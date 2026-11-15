/* xexdump: write the decrypted memory image of a basic-compressed XEX2.
 * Follows XexModule::ReadImageBasicCompressed in the ReXGlue SDK.
 * The AES key comes from key.h, generated at build time from the SDK source.
 * usage: xexdump <in.xex> <out.bin>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rijndael-alg-fst.h"
#include "key.h"

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) { perror("open"); return 1; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* x = malloc(len);
  if (!x || fread(x, 1, len, f) != (size_t)len) return 1;
  fclose(f);
  if (memcmp(x, "XEX2", 4)) { fprintf(stderr, "not XEX2\n"); return 1; }

  uint32_t hdr = be32(x + 8), sec = be32(x + 16), n = be32(x + 20), ffi = 0;
  for (uint32_t i = 0; i < n; i++)
    if (be32(x + 24 + 8 * i) == 0x3FF) ffi = be32(x + 24 + 8 * i + 4);
  if (!ffi) { fprintf(stderr, "no file format info\n"); return 1; }
  uint32_t info = be32(x + ffi);
  uint16_t enc = be16(x + ffi + 4), comp = be16(x + ffi + 6);
  uint32_t image = be32(x + sec + 4), load = be32(x + sec + 0x110);
  if (enc != 1 || comp != 1) { fprintf(stderr, "unsupported enc=%u comp=%u\n", enc, comp); return 1; }

  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t session[16];
  int nr = rijndaelKeySetupDec(rk, retail_key, 128);
  rijndaelDecrypt(rk, nr, x + sec + 0x150, session);
  nr = rijndaelKeySetupDec(rk, session, 128);

  uint8_t* out = calloc(1, image);
  uint8_t iv[16] = {0};
  const uint8_t* p = x + hdr;
  uint64_t d = 0;
  uint32_t blocks = (info - 8) / 8;
  for (uint32_t b = 0; b < blocks; b++) {
    uint32_t data = be32(x + ffi + 8 + 8 * b), zero = be32(x + ffi + 12 + 8 * b);
    if (d + data > image || (p - x) + (uint64_t)data > (uint64_t)len) {
      fprintf(stderr, "block %u out of range\n", b);
      return 1;
    }
    for (uint32_t m = 0; m < data; m += 16) {
      rijndaelDecrypt(rk, nr, p + m, out + d + m);
      for (int i = 0; i < 16; i++) {
        out[d + m + i] ^= iv[i];
        iv[i] = p[m + i];
      }
    }
    p += data;
    d += (uint64_t)data + zero;
  }

  if (out[0] != 'M' || out[1] != 'Z') { fprintf(stderr, "no MZ: decrypt failed\n"); return 1; }
  uint32_t pe = out[0x3C] | (out[0x3D] << 8) | (out[0x3E] << 16) | ((uint32_t)out[0x3F] << 24);
  if (pe + 4 > image || memcmp(out + pe, "PE\0\0", 4)) { fprintf(stderr, "no PE signature\n"); return 1; }
  uint16_t machine = out[pe + 4] | (out[pe + 5] << 8);

  f = fopen(argv[2], "wb");
  if (!f || fwrite(out, 1, image, f) != image) { perror("write"); return 1; }
  fclose(f);
  printf("%s OK load=0x%08X image=0x%X used=0x%llX blocks=%u pe=0x%X machine=0x%04X\n", argv[1], load,
         image, (unsigned long long)d, blocks, pe, machine);
  return 0;
}
