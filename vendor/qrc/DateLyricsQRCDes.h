// public domain des by brad conte (see notice)

#ifndef DATELYRICS_QRC_DES_H
#define DATELYRICS_QRC_DES_H

#include <stddef.h>

#define DES_BLOCK_SIZE 8

typedef unsigned char DateLyricsQRCByte;
typedef unsigned int  DateLyricsQRCWord;

typedef enum {
	DateLyricsQRCEncrypt,
	DateLyricsQRCDecrypt
} DateLyricsQRCMode;

void DateLyricsQRCDesKeySetup(const DateLyricsQRCByte key[], DateLyricsQRCByte schedule[][6], DateLyricsQRCMode mode);
void DateLyricsQRCDesCrypt(const DateLyricsQRCByte in[], DateLyricsQRCByte out[], const DateLyricsQRCByte key[][6]);

void DateLyricsQRCKeySetup(const DateLyricsQRCByte key[], DateLyricsQRCByte schedule[][16][6], DateLyricsQRCMode mode);
void DateLyricsQRCDecryptBlock(const DateLyricsQRCByte in[], DateLyricsQRCByte out[], const DateLyricsQRCByte key[][16][6]);

#endif // DATELYRICS_QRC_DES_H
