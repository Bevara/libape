/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / Monkey's Audio decoder filter, based on the
 *  Monkey's Audio SDK (https://github.com/fernandotcl/monkeys-audio), a
 *  standalone decoder - no ffmpeg involved.
 *
 *  The library reads through its own CIO abstraction, so the packet held in
 *  memory is wrapped in one below rather than written to a temporary file.
 */

/* gpac/filters.h pulls <emscripten/emscripten.h> in from inside its own
 * `extern "C" {` block, which is fine for the plain-C filters but breaks under
 * C++: em_asm.h defines templates, and templates cannot have C linkage.
 * Including it first here (its #pragma once then makes filters.h's copy a
 * no-op) keeps it out of that block - same workaround as poppler/dec_pdf.cpp. */
#include <emscripten/emscripten.h>

#include <gpac/filters.h>
#include <gpac/constants.h>

#include <string.h>
#include <stdlib.h>

#include <MAC/All.h>
#include <MAC/MACLib.h>
#include <MAC/IO.h>
#include <MAC/APEInfo.h>

/* The SDK compiles the destructor of APE_FILE_INFO as a weak symbol, and the
 * archive member holding it is not pulled into a side-module link - the module
 * then imports it from a host that does not have it and fails to instantiate.
 * Instantiating the structure here emits the destructor in this translation
 * unit instead. */
static void apedec_emit_weak_symbols(void)
{
	APE_FILE_INFO info;
	(void)info;
}

/* The SDK's filename handling pulls in wide-character libc calls that the
 * minimal solver does not export. The filter never passes a filename - it
 * decodes from memory through CIO - so these only have to be present, not
 * locale-aware. Defining them here keeps the module loadable next to
 * solver_minimal_1, like every other filter in this repo.
 *
 * They are deliberately ASCII-only: anything else would be dead code here. */
extern "C" {

size_t wcslen(const wchar_t *s)
{
	const wchar_t *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

int wcscasecmp(const wchar_t *a, const wchar_t *b)
{
	for (; *a && *b; a++, b++)
	{
		wchar_t ca = (*a >= L'A' && *a <= L'Z') ? (wchar_t)(*a + 32) : *a;
		wchar_t cb = (*b >= L'A' && *b <= L'Z') ? (wchar_t)(*b + 32) : *b;
		if (ca != cb)
			return (int)(ca - cb);
	}
	return (int)(*a - *b);
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
{
	size_t i = 0;
	for (; src[i] && (!dst || i < n); i++)
	{
		if (dst)
			dst[i] = (wchar_t)(unsigned char)src[i];
	}
	if (dst && (i < n))
		dst[i] = 0;
	return i;
}

char *setlocale(int category, const char *locale)
{
	(void)category;
	(void)locale;
	return NULL;
}

}

/* Monkey's Audio reports some failures by throwing, and the host solver does
 * not export __cxa_throw (checked on solver_1 and solver_minimal_1), so the
 * side module would not instantiate without this definition. A file that makes
 * the SDK throw therefore ends the module rather than returning an error;
 * well formed files never reach it. */
extern "C" void __cxa_throw(void *thrown_exception, void *tinfo, void (*dest)(void *))
{
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[APEDec] Monkey's Audio raised an exception, aborting the module\n"));
	abort();
}

/* CIO over the packet: only the read side is implemented, the decoder never
 * writes. Seek follows the usual SEEK_SET/CUR/END convention. */
class GF_APEMemIO : public CIO
{
public:
	GF_APEMemIO(const unsigned char *data, unsigned int size) : m_data(data), m_size(size), m_pos(0) {}
	virtual ~GF_APEMemIO() {}

	int Open(const wchar_t *) { return 0; }
	int Close() { return 0; }

	int Read(void *buffer, unsigned int bytes, unsigned int *read)
	{
		unsigned int avail = m_size - m_pos;
		if (bytes > avail)
			bytes = avail;
		if (bytes)
			memcpy(buffer, m_data + m_pos, bytes);
		m_pos += bytes;
		if (read)
			*read = bytes;
		return 0;
	}
	int Write(const void *, unsigned int, unsigned int *written)
	{
		if (written)
			*written = 0;
		return -1;
	}
	int Seek(int distance, unsigned int mode)
	{
		long long base = (mode == SEEK_SET) ? 0 : (mode == SEEK_CUR) ? (long long)m_pos : (long long)m_size;
		long long target = base + distance;
		if ((target < 0) || (target > (long long)m_size))
			return -1;
		m_pos = (unsigned int)target;
		return 0;
	}
	int Create(const wchar_t *) { return -1; }
	int Delete() { return -1; }
	int SetEOF() { return -1; }
	int GetPosition() { return (int)m_pos; }
	int GetSize() { return (int)m_size; }
	int GetName(wchar_t *) { return -1; }

private:
	const unsigned char *m_data;
	unsigned int m_size, m_pos;
};

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_APEDecCtx;

/* PROP_UINT()/PROP_LONGUINT() take the address of a C99 compound literal,
 * which C++ has no equivalent for; these give the value real storage instead
 * (same approach as poppler/dec_pdf.cpp). */
static void apedec_set_uint(GF_FilterPid *pid, u32 prop_code, u32 val)
{
	GF_PropertyValue pv;
	pv.type = GF_PROP_UINT;
	pv.value.uint = val;
	gf_filter_pid_set_property(pid, prop_code, &pv);
}

static void apedec_set_longuint(GF_FilterPid *pid, u32 prop_code, u64 val)
{
	GF_PropertyValue pv;
	pv.type = GF_PROP_LUINT;
	pv.value.longuint = val;
	gf_filter_pid_set_property(pid, prop_code, &pv);
}

static GF_Err apedec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_APEDecCtx *ctx = (GF_APEDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	apedec_set_uint(ctx->opid, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO);
	apedec_set_uint(ctx->opid, GF_PROP_PID_CODECID, GF_CODECID_RAW);
	apedec_set_uint(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, GF_AUDIO_FMT_S16);
	apedec_set_uint(ctx->opid, GF_PROP_PID_SAMPLE_RATE, 44100);
	apedec_set_uint(ctx->opid, GF_PROP_PID_TIMESCALE, 44100);
	apedec_set_uint(ctx->opid, GF_PROP_PID_NUM_CHANNELS, 2);

	return GF_OK;
}

static Bool apedec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_APEDecCtx *ctx = (GF_APEDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err apedec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size;
	GF_APEDecCtx *ctx = (GF_APEDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	{
		GF_APEMemIO io(data, size);
		apedec_emit_weak_symbols();
		int err = 0;
		IAPEDecompress *dec = CreateIAPEDecompressEx(&io, &err);
		if (!dec)
		{
			gf_filter_pid_drop_packet(ctx->ipid);
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[APEDec] Not a Monkey's Audio file (error %d)\n", err));
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		u32 sample_rate = (u32)dec->GetInfo(APE_INFO_SAMPLE_RATE);
		u32 channels = (u32)dec->GetInfo(APE_INFO_CHANNELS);
		u32 bytes_per_sample = (u32)dec->GetInfo(APE_INFO_BYTES_PER_SAMPLE);
		u32 block_align = (u32)dec->GetInfo(APE_INFO_BLOCK_ALIGN);
		s64 total_blocks = (s64)dec->GetInfo(APE_DECOMPRESS_TOTAL_BLOCKS);

		if (!sample_rate || !channels || (total_blocks <= 0) || !block_align)
		{
			delete dec;
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		char *raw = (char *)gf_malloc((size_t)total_blocks * block_align);
		if (!raw)
		{
			delete dec;
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_OUT_OF_MEM;
		}

		s64 done = 0;
		while (done < total_blocks)
		{
			int got = 0;
			int chunk = (int)((total_blocks - done > 4096) ? 4096 : (total_blocks - done));
			if (dec->GetData(raw + done * block_align, chunk, &got) != 0)
				break;
			if (got <= 0)
				break;
			done += got;
		}
		delete dec;
		gf_filter_pid_drop_packet(ctx->ipid);

		if (!done)
		{
			gf_free(raw);
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[APEDec] File decoded to no audio\n"));
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		apedec_set_uint(ctx->opid, GF_PROP_PID_SAMPLE_RATE, sample_rate);
		apedec_set_uint(ctx->opid, GF_PROP_PID_TIMESCALE, sample_rate);
		apedec_set_uint(ctx->opid, GF_PROP_PID_NUM_CHANNELS, channels);
		apedec_set_longuint(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, (channels == 1) ? GF_AUDIO_CH_FRONT_CENTER : (GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT));

		u32 out_samples = (u32)(done * channels);
		dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_samples * (u32)sizeof(s16), &output);
		if (!dst_pck)
		{
			gf_free(raw);
			return GF_OUT_OF_MEM;
		}

		/* The library returns native WAV samples: 8-bit unsigned, or 16/24-bit
		 * signed little endian. Everything is brought back to 16-bit signed. */
		if (bytes_per_sample == 2)
		{
			memcpy(output, raw, out_samples * sizeof(s16));
		}
		else
		{
			s16 *dst = (s16 *)output;
			for (u32 i = 0; i < out_samples; i++)
			{
				const unsigned char *p = (const unsigned char *)raw + (size_t)i * bytes_per_sample;
				if (bytes_per_sample == 1)
					dst[i] = (s16)(((int)p[0] - 128) << 8);
				else if (bytes_per_sample == 3)
					dst[i] = (s16)((s32)((u32)p[1] | ((u32)p[2] << 8)));
				else
					dst[i] = 0;
			}
		}
		gf_free(raw);

		gf_filter_pck_set_cts(dst_pck, 0);
		gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
		gf_filter_pck_set_duration(dst_pck, (u32)done);
		gf_filter_pck_send(dst_pck);
	}

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void apedec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability APEDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "ape|apl|mac"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/x-monkeys-audio|audio/x-ape"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister APEDecoderRegister = {
	.name = "apedec",
	GF_FS_SET_DESCRIPTION("Monkey's Audio decoder")
		GF_FS_SET_HELP("This filter decodes Monkey's Audio (.ape) files using the Monkey's Audio SDK.")
			.private_size = sizeof(GF_APEDecCtx),
	SETCAPS(APEDecCaps),
	.configure_pid = apedec_configure_pid,
	.process = apedec_process,
	.process_event = apedec_process_event,
	.finalize = apedec_finalize,
};

extern "C" {

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE apedec_register(GF_FilterSession *session)
{
	return &APEDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_apedec(void) {
    gf_filter_auto_register("apedec", apedec_register);
}

}
