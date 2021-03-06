#include "precompiled.h"

char* UTIL_VarArgs(const char* format, ...)
{
	va_list argptr;
	static char string[1024];

	va_start(argptr, format);
	Q_vsnprintf(string, sizeof string, format, argptr);
	va_end(argptr);

	return string;
}

short FixedSigned16(float value, float scale)
{
	return (short)Q_clamp(int(value * scale), SHRT_MIN, SHRT_MAX);
}

unsigned short FixedUnsigned16(float value, float scale)
{
	return (unsigned short)Q_clamp(int(value * scale), 0, USHRT_MAX);
}

void UTIL_HudMessage(edict_t* pEntity, const hudtextparms_t& textparms, const char* pMessage)
{
	if (FNullEnt(pEntity) || pEntity->free)
		return;

	MESSAGE_BEGIN(MSG_ONE, SVC_TEMPENTITY, nullptr, pEntity);
	WRITE_BYTE(TE_TEXTMESSAGE);
	WRITE_BYTE(textparms.channel & 0xFF);

	WRITE_SHORT(FixedSigned16(textparms.x, 1 << 13));
	WRITE_SHORT(FixedSigned16(textparms.y, 1 << 13));
	WRITE_BYTE(textparms.effect);

	WRITE_BYTE(textparms.r1);
	WRITE_BYTE(textparms.g1);
	WRITE_BYTE(textparms.b1);
	WRITE_BYTE(textparms.a1);

	WRITE_BYTE(textparms.r2);
	WRITE_BYTE(textparms.g2);
	WRITE_BYTE(textparms.b2);
	WRITE_BYTE(textparms.a2);

	WRITE_SHORT(FixedUnsigned16(textparms.fadeinTime, 1 << 8));
	WRITE_SHORT(FixedUnsigned16(textparms.fadeoutTime, 1 << 8));
	WRITE_SHORT(FixedUnsigned16(textparms.holdTime, 1 << 8));

	if (textparms.effect == 2)
	WRITE_SHORT(FixedUnsigned16(textparms.fxTime, 1 << 8));

	if (Q_strlen(pMessage) < 512) {
		WRITE_STRING(pMessage);
	}
	else {
		char tmp[512];
		Q_strlcpy(tmp, pMessage);
		WRITE_STRING(tmp);
	}
	MESSAGE_END();
}
