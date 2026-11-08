/* SdFat.h - HOST shim of the SdFat 2.x API subset used by TallyCCU Pro.
 * The "card" is the directory EMU_SD_DIR (begin() fails if unset/missing).
 * Semantics kept from SdFat 2.x (FAT32, File = File32):
 *  - FILE_WRITE = O_RDWR|O_CREAT|O_AT_END (opens at EOF, does NOT truncate)
 *  - rename() fails if the target exists; remove() fails on missing files
 *  - getWriteError() is true on a closed file; available() capped at 32767
 *  - openNextFile() lists entries sorted by name (FAT order not reproduced)
 * EMU_SD_BYTE_NS (default 1000 in realtime mode) paces data transfer. */
#ifndef EMU_SDFAT_H
#define EMU_SDFAT_H
#include <Arduino.h>
#include <Stream.h>

#ifndef O_RDONLY
#define O_RDONLY 0X00
#define O_WRONLY 0X01
#define O_RDWR 0X02
#endif
#define O_ACCMODE_EMU 0x03
#define O_AT_END_EMU 0X04
#define O_APPEND_EMU 0X08
#define O_CREAT_EMU 0x10
#define O_TRUNC_EMU 0x20
#define O_EXCL_EMU 0x40
#ifndef O_AT_END
#define O_AT_END O_AT_END_EMU
#endif
#define O_READ O_RDONLY
#define O_WRITE O_WRONLY
#define FILE_READ O_RDONLY
#define FILE_WRITE (O_RDWR | O_CREAT_EMU | O_AT_END_EMU)
#define SD_SCK_MHZ(maxMhz) (1000000UL * (maxMhz))
#define SPI_FULL_SPEED SD_SCK_MHZ(50)
#define SPI_HALF_SPEED SD_SCK_MHZ(4)
typedef uint8_t oflag_t;

struct EmuFileState;

class File : public Stream {
 public:
  File();
  File(const File &o);
  File &operator=(const File &o);
  ~File();
  operator bool() const { return isOpen(); }
  bool isOpen() const;
  bool isDir() const;
  bool isDirectory() { return isDir(); }
  bool isFile() const { return isOpen() && !isDir(); }
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  int read(void *buf, size_t n);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t *buf, size_t n) override;
  size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  using Print::write;
  int availableForWrite() override { return 512; }
  bool close();
  bool sync();
  uint32_t size() const;
  uint32_t fileSize() const { return size(); }
  uint32_t position() const;
  uint32_t curPosition() const { return position(); }
  bool seek(uint32_t pos);
  bool seekSet(uint32_t pos) { return seek(pos); }
  bool seekEnd(int32_t off = 0) { return seek(size() + off); }
  bool rewind() { return seek(0); }
  size_t getName(char *name, size_t size);
  const char *name() const;
  File openNextFile(oflag_t oflag = O_RDONLY);
  void rewindDirectory();
  bool getWriteError() const;
  void clearWriteError();
  bool remove();
  bool truncate(uint32_t len);
  /* internal */
  EmuFileState *_st;
};
typedef File File32;
typedef File FsFile;
typedef File SdFile;

class SdFat {
 public:
  bool begin(uint8_t csPin = 10, uint32_t maxSck = SD_SCK_MHZ(50));
  File open(const char *path, oflag_t oflag = O_RDONLY);
  File open(const String &path, oflag_t oflag = O_RDONLY) { return open(path.c_str(), oflag); }
  bool exists(const char *path);
  bool remove(const char *path);
  bool rename(const char *oldPath, const char *newPath);
  bool mkdir(const char *path, bool pFlag = true);
  bool rmdir(const char *path);
  bool chdir(const char *path = "/") { (void)path; return true; }
  uint32_t freeClusterCount() { return 100000; }
  uint8_t sectorsPerCluster() { return 64; }
  void end() { _begun = false; }
  uint8_t sdErrorCode();   /* sd-p2 test variant: sticky card error */
  bool _begun = false;
};
typedef SdFat SdFat32;
typedef SdFat SdFs;
#endif
