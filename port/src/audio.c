#include <PR/ultratypes.h>
#include <stdio.h>
#include <SDL.h>
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "record.h"
#include "system.h"

#ifdef PLATFORM_WEB
#include <emscripten.h>
#endif

#ifndef PLATFORM_WEB
static SDL_AudioDeviceID dev;
#endif
static const s16 *nextBuf;
static u32 nextSize = 0;
static s32 queueErrorReported;

#ifdef PLATFORM_WEB
// amgrFrame() normally keeps about 1100 source samples queued. Web output is
// scheduled further ahead than that so a long render or a synchronous texture
// decode cannot starve the browser's audio thread. audioGetBytesBuffered()
// subtracts the difference when reporting the queue to amgrFrame(), preserving
// its existing production cadence while keeping the extra web-only headroom.
#define WEB_AUDIO_AMGR_TARGET_SAMPLES 1100
static s32 bufferSize = 2048;
static const s32 bufferSizeMin = 2048;
static const s32 bufferSizeMax = 8192;

EM_JS(s32, webAudioInit, (s32 sourceRate, s32 leadFrames, s32 reportTarget), {
	try {
		var AudioContextClass = window.AudioContext || window.webkitAudioContext;

		if (!AudioContextClass) {
			console.error('Web Audio API is not available');
			return -1;
		}

		var context = Module['pdAudioContext'];

		if (!context || context.state === 'closed') {
			// Keep queued mixer blocks at their native rate. The browser resamples
			// the complete context output to the audio device without introducing
			// a new resampling boundary at every block.
			context = new AudioContextClass({ sampleRate: sourceRate });
			Module['pdAudioContext'] = context;
		}

		if (context.sampleRate !== sourceRate) {
			console.error('Web Audio opened at ' + context.sampleRate
				+ ' Hz; expected ' + sourceRate + ' Hz');
			return -1;
		}

		var audio = {
			context: context,
			sourceRate: sourceRate,
			leadFrames: leadFrames,
			reportTarget: reportTarget,
			nextTime: 0,
			started: false,
			resumePending: false
		};

		Module['pdAudio'] = audio;

		var resume = function () {
			if (context.state !== 'suspended' || audio.resumePending) return;
			audio.resumePending = true;
			context.resume().catch(function (error) {
				console.error('Could not resume Web Audio:', error);
			}).finally(function () {
				audio.resumePending = false;
			});
		};

		document.addEventListener('pointerdown', resume, true);
		document.addEventListener('keydown', resume, true);
		resume();
		return 0;
	} catch (error) {
		console.error('Could not initialise Web Audio:', error);
		return -1;
	}
});

EM_JS(s32, webAudioQueue, (const void *data, u32 size), {
	var audio = Module['pdAudio'];

	if (!audio || audio.context.state === 'closed') return -1;

	try {
		var frames = size >>> 2;
		var buffer = audio.context.createBuffer(2, frames, audio.sourceRate);
		var left = buffer.getChannelData(0);
		var right = buffer.getChannelData(1);
		var input = data >>> 1;

		for (var i = 0; i < frames; i++) {
			left[i] = HEAP16[input++] / 32768;
			right[i] = HEAP16[input++] / 32768;
		}

		var now = audio.context.currentTime;
		var start = audio.nextTime;

		if (!audio.started || start < now) {
			start = now + audio.leadFrames / audio.sourceRate;
			audio.started = true;
		}

		var source = audio.context.createBufferSource();
		source.buffer = buffer;
		source.connect(audio.context.destination);
		source.onended = function () {
			source.disconnect();
		};
		source.start(start);
		audio.nextTime = start + buffer.duration;
		return 0;
	} catch (error) {
		console.error('Could not queue Web Audio:', error);
		return -1;
	}
});

EM_JS(u32, webAudioGetBytesBuffered, (), {
	var audio = Module['pdAudio'];

	if (!audio) return 0;

	var frames = Math.max(0,
		Math.floor((audio.nextTime - audio.context.currentTime) * audio.sourceRate));
	var extra = Math.max(0, audio.leadFrames - audio.reportTarget);
	var reported = Math.max(0, frames - extra);
	return Math.min(reported * 4, 0xffffffff) >>> 0;
});
#else
static s32 bufferSize = 512;
static const s32 bufferSizeMin = 0;
static const s32 bufferSizeMax = 1 * 1024 * 1024;
#endif
static s32 queueLimit = 8192;
static s32 sampleRate = 22020;

s32 audioInit(void)
{
	nextBuf = NULL;
	queueErrorReported = 0;

#ifdef PLATFORM_WEB
	if (webAudioInit(sampleRate, bufferSize, WEB_AUDIO_AMGR_TARGET_SAMPLES) != 0) {
		sysLogPrintf(LOG_ERROR, "Web Audio init error");
		return -1;
	}
#else
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 22020; // TODO: this might cause trouble for some platforms
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = bufferSize;
	want.callback = NULL;

	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (dev == 0) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudio error: %s", SDL_GetError());
		return -1;
	}

	SDL_PauseAudioDevice(dev, 0);

	sampleRate = have.freq;
#endif

	return 0;
}

s32 audioGetSampleRate(void)
{
	return sampleRate;
}

s32 audioGetBytesBuffered(void)
{
#ifdef PLATFORM_WEB
	return webAudioGetBytesBuffered();
#else
	return SDL_GetQueuedAudioSize(dev);
#endif
}

s32 audioGetSamplesBuffered(void)
{
	return audioGetBytesBuffered() / 4;
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
	nextBuf = buf;
	nextSize = len;
}

void audioEndFrame(void)
{
	if (nextBuf && nextSize) {
		if (audioGetSamplesBuffered() < queueLimit) {
			s32 queued;

#ifdef PLATFORM_WEB
			queued = webAudioQueue(nextBuf, nextSize);
#else
			queued = SDL_QueueAudio(dev, nextBuf, nextSize);
#endif

			if (queued == 0) {
				// Inside the check on purpose: a recording should hold what was
				// played, and a buffer dropped for a full queue was not.
				recordPushAudio(nextBuf, nextSize);
			} else if (!queueErrorReported) {
#ifdef PLATFORM_WEB
				sysLogPrintf(LOG_ERROR, "Web Audio queue error");
#else
				sysLogPrintf(LOG_ERROR, "SDL_QueueAudio error: %s", SDL_GetError());
#endif
				queueErrorReported = 1;
			}
		}
		nextBuf = NULL;
		nextSize = 0;
	}
}

PD_CONSTRUCTOR static void audioConfigInit(void)
{
#ifdef PLATFORM_WEB
	configRegisterInt("Audio.WebBufferSize", &bufferSize, bufferSizeMin, bufferSizeMax);
#else
	configRegisterInt("Audio.BufferSize", &bufferSize, bufferSizeMin, bufferSizeMax);
#endif
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 1 * 1024 * 1024);
}
