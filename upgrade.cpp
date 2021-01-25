// Copyright (c) 2021 - Schelte Bron

#include <Ticker.h>
#include "otgwmcu.h"
#include "proxy.h"

#define STX 0x0F
#define ETX 0x04
#define DLE 0x05

#define byteswap(val) ((val << 8) | (val >> 8))

enum {
  FWSTATE_IDLE,
  FWSTATE_RSET,
  FWSTATE_VERSION,
  FWSTATE_PREP,
  FWSTATE_CODE,
  FWSTATE_DATA
};

enum {
  CMD_VERSION,
  CMD_READPROG,
  CMD_WRITEPROG,
  CMD_ERASEPROG,
  CMD_READDATA,
  CMD_WRITEDATA,
  CMD_READCFG,
  CMD_WRITECFG,
  CMD_RESET
};

enum {
  ERROR_NONE,
  ERROR_READFILE,
  ERROR_MAGIC,
  ERROR_RESET,
  ERROR_RETRIES,
  ERROR_MISMATCHES
};

static byte fwstate = FWSTATE_IDLE;

struct fwupdatedata {
  unsigned char buffer[80];
  unsigned char datamem[256];
  unsigned short codemem[4096];
  unsigned short failsafe[4];
  unsigned short prot[2];
  unsigned short errcnt, retries;
} *fwupd;

Ticker timeout;

void toggle() {
  int state = digitalRead(LED1);
  digitalWrite(LED1, !state);
}

void blink(short delayms = 200) {
  static Ticker blinker;
  if (delayms) {
    blinker.attach_ms(delayms, toggle);
  } else {
    blinker.detach();
  }
}

void picreset() {
  pinMode(PICRST,OUTPUT);
  digitalWrite(PICRST,LOW);
  delay(100);
  digitalWrite(PICRST,HIGH);
  pinMode(PICRST,INPUT);
}

unsigned char hexcheck(char *hex, int len) {
  unsigned char sum = 0;
  int val;

  while (len-- > 0) {
    sscanf(hex, "%02x", &val);
    sum -= val;
    hex += 2;
  }
  return sum;
}

bool readhex(unsigned short *codemem, unsigned char *datamem, unsigned short *config = nullptr) {
  char hexbuf[48];
  int len, addr, tag, data, offs, linecnt = 0;
  bool rc = false;
  File f;

  f = LittleFS.open("/gateway.hex", "r");
  if (!f) return false;
  memset(codemem, -1, 4096 * sizeof(short));
  memset(datamem, -1, 256 * sizeof(char));
  f.setTimeout(0);
  while (f.readBytesUntil('\n', hexbuf, sizeof(hexbuf)) != 0) {
    linecnt++;
    if (sscanf(hexbuf, ":%2x%4x%2x", &len, &addr, &tag) != 3) {
      // Parse error
      Serial.println(hexbuf);
      break;
    }
    if (len & 1) {
      // Invalid data size
      Serial.println("Invalid data size");
      break;
    }
    if (hexcheck(hexbuf + 1, len + 5) != 0) {
      // Checksum error
      Serial.println("Checksum error");
      break;
    }
    offs = 9;
    len >>= 1;
    if (tag == 0) {
      if (addr >= 0x4400) {
        // Bogus addresses
        continue;
      } else if (addr >= 0x4200) {
        // Data memory
        addr = (addr - 0x4200) >> 1;
        while (len > 0) {
          if (sscanf(hexbuf + offs, "%04x", &data) != 1) break;
          datamem[addr++] = byteswap(data);
          offs += 4;
          len--;
        }
      } else if (addr >= 0x4000) {
        // Configuration bits
        if (config == nullptr) continue;
        addr = (addr - 0x4000) >> 1;
        while (len > 0) {
          if (sscanf(hexbuf + offs, "%04x", &data) != 1) break;
          config[addr++] = byteswap(data);
          offs += 4;
          len--;
        }
      } else {
        // Program memory
        addr >>= 1;
        while (len > 0) {
          if (sscanf(hexbuf + offs, "%04x", &data) != 1) {
            Serial.printf("Didn't find hex data at offset %d\n", offs);
            break;
          }
          codemem[addr++] = byteswap(data);
          offs += 4;
          len--;
        }
      }
      if (len) break;
    } else if (tag == 1) {
      rc = true;
      break;
    }
  }
  f.close();
  return rc;
}

void fwupgradefail();

void fwupgradecmd(const unsigned char *cmd, int len) {
  byte i, ch, sum = 0;

  Serial.write(STX);
  for (i = 0; i <= len; i++) {
    ch = i < len ? cmd[i] : sum;
    if (ch == STX || ch == ETX || ch == DLE) {
      Serial.write(DLE);
    }
    Serial.write(ch);
    sum -= ch;
  }
  Serial.write(ETX);
}

bool erasecode(short addr) {
  byte fwcommand[] = {CMD_ERASEPROG, 1, 0, 0};
  bool rc = false;
  short i;
  for (i = 0; i < 32; i++) {
    if (fwupd->codemem[addr + i] != 0xffff) {
      rc = true;
      break;
    }
  }
  if (rc) {
    fwcommand[2] = addr & 0xff;
    fwcommand[3] = addr >> 8;
    fwupgradecmd(fwcommand, sizeof(fwcommand));
  }
  return rc;
}

void loadcode(short addr, const unsigned short *code, short len = 32) {
  byte i, fwcommand[4 + 2 * len];
  unsigned short *data = (unsigned short *)fwcommand + 2;
  fwcommand[0] = CMD_WRITEPROG;
  fwcommand[1] = len >> 2;
  fwcommand[2] = addr & 0xff;
  fwcommand[3] = addr >> 8;
  for (i = 0; i < len; i++) {
    data[i] = code[i] & 0x3fff;
  }
  fwupgradecmd(fwcommand, sizeof(fwcommand));  
}

void readcode(short addr, short len = 32) {
  byte fwcommand[] = {CMD_READPROG, 32, 0, 0};
  fwcommand[1] = len;
  fwcommand[2] = addr & 0xff;
  fwcommand[3] = addr >> 8;
  fwupgradecmd(fwcommand, sizeof(fwcommand));
}

bool verifycode(const unsigned short *code, const unsigned short *data, short len = 32) {
  short i;
  bool rc = true;

  for (i = 0; i < len; i++) {
    if (data[i] != (code[i] & 0x3fff)) {
      fwupd->errcnt++;
      rc = false;
    }
  }
  return rc;
}

void loaddata(short addr) {
  byte i;
  byte fwcommand[68] = {CMD_WRITEDATA, 64};
  fwcommand[2] = addr & 0xff;
  fwcommand[3] = addr >> 8;
  for (i = 0; i < 64; i++) {
    fwcommand[i + 4] = fwupd->datamem[addr + i];
  }
  fwupgradecmd(fwcommand, sizeof(fwcommand));  
}

void readdata(short addr) {
  byte fwcommand[] = {CMD_READDATA, 64, 0, 0};
  fwcommand[2] = addr & 0xff;
  fwupgradecmd(fwcommand, sizeof(fwcommand));
}

bool verifydata(short pc, const byte *data, short len = 64) {
  short i;
  bool rc = true;

  for (i = 0; i < len; i++) {
    if (data[i] != fwupd->datamem[pc + i]) {
      fwupd->errcnt++;
      rc = false;
    }
  }
  return rc;
}

void fwupgradestop(int result) {
  free((void *)fwupd);
  fwupd = nullptr;
  fwstate = FWSTATE_IDLE;
  timeout.detach();
  if (result == ERROR_NONE) {
    digitalWrite(LED1, HIGH);
  } else if (result == ERROR_READFILE) {
    blink(500);
  } else if (result == ERROR_MAGIC) {
    blink(500);
  } else {
    blink(100);
  }
}

void fwupgradestep(const byte *packet = nullptr, int len = 0) {
  const unsigned short *data = (const unsigned short *)packet;
  static short pc;
  static byte lastcmd = 0;
  byte cmd = 0;

  if (packet == nullptr || len == 0) {
    cmd = lastcmd;
  } else {
    cmd = packet[0];
    lastcmd = cmd;
  }

  switch (fwstate) {
    case FWSTATE_IDLE:
      fwupd->errcnt = 0;
      fwupd->retries = 0;
      picreset();
      fwstate = FWSTATE_RSET;
      break;
    case FWSTATE_RSET:
      if (packet != nullptr) {
        byte fwcommand[] = {CMD_VERSION, 3};
        fwupgradecmd(fwcommand, sizeof(fwcommand));
        fwstate = FWSTATE_VERSION;
      } else if (++fwupd->retries > 5) {
        fwupgradestop(ERROR_RETRIES);
      } else {
        // Serial.print("\rGW=R\r");
        picreset();
      }
      break;
    case FWSTATE_VERSION:
      if (data != nullptr) {
        fwupd->prot[0] = data[2];
        fwupd->prot[1] = data[3];
        fwupd->failsafe[0] = 0x158a;                          // BSF PCLATH,3
        if ((fwupd->prot[0] & 0x800) == 0) {
          fwupd->failsafe[0] = 0x118a;                        // BCF PCLATH,3
        }
        fwupd->failsafe[1] = 0x2000 | fwupd->prot[0] & 0x7ff; // CALL SelfProg
        fwupd->failsafe[2] = 0x118a;                          // BCF PCLATH,3
        fwupd->failsafe[3] = 0x2820;                          // GOTO 0x20
        pc = 0x20;
        erasecode(pc);
        fwstate = FWSTATE_PREP;
      } else if (++fwupd->retries > 10) {
        fwupgradestop(ERROR_RETRIES);
      } else {
        byte fwcommand[] = {CMD_VERSION, 3};
        fwupgradecmd(fwcommand, sizeof(fwcommand));
        fwstate = FWSTATE_VERSION;
      }
      break;
    case FWSTATE_PREP:
      if (cmd == CMD_ERASEPROG) {
        loadcode(pc, fwupd->failsafe, 4);
      } else if (cmd == CMD_WRITEPROG) {
        readcode(pc, 4);
      } else {
        if (packet != nullptr && packet[1] == 4 && data[1] == pc && verifycode(fwupd->failsafe, data + 2, 4)) {
          pc = 0;
          erasecode(pc);
          fwstate = FWSTATE_CODE;
        } else {
          // Failed. Try again.
          if (++fwupd->retries > 5) {
            fwupgradestop(ERROR_RETRIES);
          } else {
            erasecode(pc);
          }
        }
      }
      break;
    case FWSTATE_CODE:
      if (cmd == CMD_ERASEPROG) {
        // digitalWrite(LED2, LOW);
        loadcode(pc, fwupd->codemem + pc);
      } else if (cmd == CMD_WRITEPROG) {
        // digitalWrite(LED2, HIGH);
        readcode(pc);
      } else if (cmd == CMD_READPROG) {
        if (packet != nullptr && packet[1] == 32 && data[1] == pc && verifycode(fwupd->codemem + pc, data + 2)) {
          do {
            do {
              pc += 32;
            } while (pc + 31 >= fwupd->prot[0] && pc <= fwupd->prot[1]);
            if (pc >= 0x1000) {
              pc = 0;
              loaddata(pc);
              fwstate = FWSTATE_DATA;
              break;
            } else if (erasecode(pc)) {
              break;
            }
          } while (pc < 0x1000);
        } else {
          if (++fwupd->retries > 100) {
            fwupgradestop(ERROR_RETRIES);
          } else {
            erasecode(pc);
          }
        }
      }
      break;
    case FWSTATE_DATA:
      if (cmd == CMD_WRITEDATA) {
        // digitalWrite(LED2, HIGH);
        readdata(pc);
      } else if (cmd == CMD_READDATA) {
        if (packet != nullptr && verifydata(pc, packet + 4)) {
          pc += 64;
          if (pc < 0x100) {
            // digitalWrite(LED2, LOW);
            loaddata(pc);
          } else {
            byte fwcommand[] = {CMD_RESET, 0};
            fwupgradecmd(fwcommand, sizeof(fwcommand));
            fwupgradestop(ERROR_NONE);
          }
        } else if (++fwupd->retries > 100) {
          fwupgradestop(ERROR_RETRIES);
        } else {
          // digitalWrite(LED2, LOW);
          loaddata(pc);
        }
      }
      break;
  }
  if (fwstate == FWSTATE_IDLE) {
    timeout.detach();
  } else {
    timeout.once_ms_scheduled(1000, fwupgradefail);
  }
}

void fwupgradestart() {
  fwupd = (struct fwupdatedata *)malloc(sizeof(struct fwupdatedata));
  if (!readhex(fwupd->codemem, fwupd->datamem)) {
    fwupgradestop(ERROR_READFILE);
    return;
  }

  if (fwupd->codemem[0] != 0x158a || (fwupd->codemem[1] & 0x3e00) != 0x2600) {
    // Bad magic
    fwupgradestop(ERROR_MAGIC);
    return;
  }

  fwupgradestep();
}

void fwupgradefail() {
  // Send a non-DLE byte in case the PIC is waiting for a byte following DLE
  Serial.write(STX);
  fwupgradestep();
}

void upgradeevent() {
  static unsigned int pressed = 0;
  static bool dle = false;
  static byte len, sum;
  int ch;

  if (fwstate == FWSTATE_IDLE) {
    if (digitalRead(BUTTON) == 0) {
      if (pressed == 0) {
        // In the very unlikely case that millis() happens to be 0, the 
        // user will have to press the button for one millisecond longer.
        pressed = millis();
      } else if (millis() - pressed > 2000) {
        blink(0);
        digitalWrite(LED1, LOW);
        fwupgradestart();
        pressed = 0;
      }
    } else {
      pressed = 0;
      proxyevent();
    }
  } else if (Serial.available() > 0) {
    ch = Serial.read();
    if (!dle && ch == STX) {
      digitalWrite(LED2, LOW);
      len = 0;
      sum = 0;
    } else if (!dle && ch == DLE) {
      dle = true;
    } else if (!dle && ch == ETX) {
      digitalWrite(LED2, HIGH);
      if (sum == 0) {
        fwupgradestep(fwupd->buffer, len);
      } else {
        fwupgradestep();
      }
      len = 0;
    } else if (fwstate != FWSTATE_RSET) {
      fwupd->buffer[len++] = ch;
      sum -= ch;
      dle = false;
    }
  }
}