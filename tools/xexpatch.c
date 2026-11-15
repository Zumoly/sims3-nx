/* xexpatch: apply a title update's XEX delta patch (.xexp) to a decrypted base image, the way
 * XexModule::ApplyPatch does in the ReXGlue SDK (header delta, then image delta blocks, LZX delta via libmspack).
 * The base image comes from xexdump. AES key from key.h, generated from the SDK source at build time.
 * usage: xexpatch <base.xex> <base.bin> <patch.xexp> <out.bin>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "rijndael-alg-fst.h"
#include "key.h"
#include "mspack.h"
#include "lzx.h"

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint8_t* load(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  *len = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* b = malloc(*len + 16);
  if (fread(b, 1, *len, f) != *len) { perror("read"); exit(1); }
  fclose(f);
  return b;
}

/* Optional header value (or offset) by key. */
static uint32_t opt(const uint8_t* x, uint32_t key) {
  uint32_t n = be32(x + 20);
  for (uint32_t i = 0; i < n; i++)
    if (be32(x + 24 + 8 * i) == key) return be32(x + 24 + 8 * i + 4);
  return 0;
}

static void ecb_decrypt(const uint8_t* key, const uint8_t* in, uint8_t* out) {
  uint32_t rk[4 * (MAXNR + 1)];
  int nr = rijndaelKeySetupDec(rk, key, 128);
  rijndaelDecrypt(rk, nr, in, out);
}

/* mspack over memory buffers (as lzx.cpp in the SDK). */
typedef struct { void* buffer; off_t size, offset; } memfile;
static int mread(struct mspack_file* f, void* b, int n) {
  memfile* m = (memfile*)f;
  off_t t = m->size - m->offset < n ? m->size - m->offset : n;
  memcpy(b, (uint8_t*)m->buffer + m->offset, t);
  m->offset += t;
  return (int)t;
}
static int mwrite(struct mspack_file* f, void* b, int n) {
  memfile* m = (memfile*)f;
  off_t t = m->size - m->offset < n ? m->size - m->offset : n;
  memcpy((uint8_t*)m->buffer + m->offset, b, t);
  m->offset += t;
  return (int)t;
}
static void* malloc_(struct mspack_system* s, size_t n) { (void)s; return calloc(n, 1); }
static void free_(void* p) { free(p); }
static void copy_(void* s, void* d, size_t n) { memmove(d, s, n); }
static struct mspack_system sys = {0};

static int lzx_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len, uint32_t window,
                          const uint8_t* ref, size_t ref_len) {
  int bits = 0;
  while ((1u << bits) < window) bits++;
  memfile in = {(void*)src, (off_t)src_len, 0}, out = {dst, (off_t)dst_len, 0};
  struct lzxd_stream* z = lzxd_init(&sys, (struct mspack_file*)&in, (struct mspack_file*)&out, bits, 0, 0x8000,
                                    (off_t)dst_len, 0);
  if (!z) return 1;
  if (ref) {
    size_t pad = window - ref_len;
    memset(&z->window[0], 0, pad);
    memcpy(&z->window[pad], ref, ref_len);
    z->ref_data_size = window;
  }
  int r = lzxd_decompress(z, (off_t)dst_len);
  lzxd_free(z);
  return r;
}

/* xex2_delta_patch: old_addr, new_addr (be32), uncompressed_len, compressed_len (be16), data. */
static int delta_apply(const uint8_t* patch, size_t len, uint32_t window, uint8_t* dest, size_t dest_len) {
  const uint8_t* end = patch + len;
  const uint8_t* p = patch;
  while (p + 12 <= end) {
    uint32_t old_addr = be32(p), new_addr = be32(p + 4);
    uint16_t ulen = be16(p + 8), clen = be16(p + 10);
    if (!old_addr && !new_addr && !ulen && !clen) break;
    if ((size_t)new_addr + ulen > dest_len || (size_t)old_addr + ulen > dest_len) {
      fprintf(stderr, "patch entry out of range: old %X new %X len %X\n", old_addr, new_addr, ulen);
      return 20;
    }
    int size = -4;
    if (clen == 0) {
      memset(dest + new_addr, 0, ulen);
    } else if (clen == 1) {
      memmove(dest + new_addr, dest + old_addr, ulen);
    } else {
      size = clen - 4;
      uint8_t* ref = malloc(ulen);
      memcpy(ref, dest + old_addr, ulen);
      int r = lzx_decompress(p + 12, clen, dest + new_addr, ulen, window, ref, ulen);
      free(ref);
      if (r) { fprintf(stderr, "lzx error %d at new %X\n", r, new_addr); return r; }
    }
    p += 16 + size;
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 5) { fprintf(stderr, "usage: xexpatch base.xex base.bin patch.xexp out.bin\n"); return 2; }
  sys.read = mread; sys.write = mwrite; sys.alloc = malloc_; sys.free = free_; sys.copy = copy_;
  size_t base_len, image_len, xp_len;
  uint8_t* base = load(argv[1], &base_len);
  uint8_t* orig_image = load(argv[2], &image_len);
  uint8_t* xp = load(argv[3], &xp_len);
  if (memcmp(base, "XEX2", 4) || memcmp(xp, "XEX2", 4)) { fprintf(stderr, "not XEX2\n"); return 1; }

  uint32_t desc = opt(xp, 0x5FF), xffi = opt(xp, 0x3FF);
  if (!desc || !xffi) { fprintf(stderr, "no delta descriptor / file format in patch\n"); return 1; }
  const uint8_t* d = xp + desc;
  uint32_t target_headers = be32(d + 0x30), hsrc_off = be32(d + 0x34), hsrc_size = be32(d + 0x38),
           htgt_off = be32(d + 0x3C), isrc_off = be32(d + 0x40), isrc_size = be32(d + 0x44), itgt_off = be32(d + 0x48);
  uint32_t window = be32(xp + xffi + 8);
  uint16_t xenc = be16(xp + xffi + 4), xcomp = be16(xp + xffi + 6);
  printf("patch: window %X enc %u comp %u, headers %X (src %X+%X -> %X), image src %X+%X -> %X\n", window, xenc, xcomp,
         target_headers, hsrc_off, hsrc_size, htgt_off, isrc_off, isrc_size, itgt_off);

  /* Header. */
  uint32_t base_hdr = be32(base + 8);
  uint32_t orig_image_size = be32(base + be32(base + 16) + 4);
  uint32_t hsize = target_headers ? target_headers : htgt_off + hsrc_size;
  uint32_t buf = hsize > base_hdr ? hsize : base_hdr;
  uint8_t* hb = calloc(1, buf + 0x10000);
  memcpy(hb, base, base_hdr);
  if (hsrc_off) memmove(hb + htgt_off, hb + hsrc_off, hsrc_size);
  if (hsize < base_hdr) memset(hb + hsize, 0, base_hdr - hsize);
  int r = delta_apply(d + 0x4C, be16(d + 0x4C + 10) + 0xC, window, hb, buf);
  if (r) { fprintf(stderr, "header patch failed %d\n", r); return 1; }
  uint32_t sec = be32(hb + 16);
  uint32_t new_image_size = be32(hb + sec + 4);
  printf("image size %X -> %X\n", orig_image_size, new_image_size);

  /* Keys. */
  uint8_t base_session[16], patch_session[16], test[16], orig_session[16];
  ecb_decrypt(retail_key, base + be32(base + 16) + 0x150, orig_session);
  ecb_decrypt(retail_key, hb + sec + 0x150, base_session);
  ecb_decrypt(base_session, xp + be32(xp + 16) + 0x150, patch_session);
  ecb_decrypt(base_session, d + 0x20, test);
  if (memcmp(test, orig_session, 16)) {
    fprintf(stderr, "image key mismatch; header changed %d, sec %X -> %X\n", memcmp(hb, base, base_hdr) != 0,
            be32(base + 16), sec);
    for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", orig_session[i]);
    fprintf(stderr, " orig\n");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", test[i]);
    fprintf(stderr, " test\n");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", base_session[i]);
    fprintf(stderr, " base session (warning only: the kernel does not check this)\n");
  }

  /* Image. */
  uint32_t cap = new_image_size > orig_image_size ? new_image_size : orig_image_size;
  uint8_t* img = calloc(1, cap);
  memcpy(img, orig_image, image_len < orig_image_size ? image_len : orig_image_size);
  if (isrc_off) memmove(img + itgt_off, img + isrc_off, isrc_size);
  uint32_t itgt = itgt_off + isrc_size;
  if (itgt < orig_image_size) memset(img + itgt, 0, orig_image_size - itgt);

  uint32_t xhdr = be32(xp + 8);
  size_t plen = xp_len - xhdr;
  uint8_t* in = malloc(plen + 16);
  if (xenc == 1) {
    uint32_t rk[4 * (MAXNR + 1)];
    int nr = rijndaelKeySetupDec(rk, patch_session, 128);
    uint8_t iv[16] = {0};
    for (size_t m = 0; m + 16 <= plen; m += 16) {
      rijndaelDecrypt(rk, nr, xp + xhdr + m, in + m);
      for (int i = 0; i < 16; i++) { in[m + i] ^= iv[i]; iv[i] = xp[xhdr + m + i]; }
    }
  } else {
    memcpy(in, xp + xhdr, plen);
  }
  uint32_t block_size = be32(xp + xffi + 12);
  const uint8_t* p = in;
  int blocks = 0;
  fprintf(stderr, "first block %X, plen %zX, first words %08X %08X %08X %08X\n", block_size, plen, be32(in), be32(in + 24),
          be32(in + 28), be32(in + 32));
  while (block_size) {
    if ((size_t)(p - in) + block_size > plen) { fprintf(stderr, "block past end\n"); return 1; }
    uint32_t next = be32(p);
    r = delta_apply(p + 24, block_size - 24, window, img, cap);
    if (r) { fprintf(stderr, "image patch failed %d in block %d\n", r, blocks); return 1; }
    p += block_size;
    block_size = next;
    blocks++;
  }
  FILE* f = fopen(argv[4], "wb");
  fwrite(img, 1, new_image_size, f);
  fclose(f);
  printf("%s: %d blocks, image %X, MZ %s\n", argv[4], blocks, new_image_size, img[0] == 'M' && img[1] == 'Z' ? "ok" : "MISSING");
  return 0;
}
