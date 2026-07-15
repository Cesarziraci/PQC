CONTIKI_PROJECT = node_main server_main
all: $(CONTIKI_PROJECT)

APP_SRC = src
COMMON_SRC = src/common
TEMP_SRC = src/Temp
PQC_SRC = src/PQC
METRICS_SRC = src/metrics

HQC_DIR = extern/HQC-Round4/Additional_Implementation/hqc-128/src
HQC_FIPS202_DIR = extern/HQC-Round4/Additional_Implementation/hqc-128/lib/fips202
AES_DIR = extern/tiny_aes

PROJECTDIRS += $(APP_SRC) $(COMMON_SRC) $(TEMP_SRC) $(PQC_SRC) $(METRICS_SRC)
PROJECTDIRS += $(HQC_DIR) $(HQC_FIPS202_DIR) $(AES_DIR)

PROJECT_SOURCEFILES += nack.c
PROJECT_SOURCEFILES += temp_measurement.c
PROJECT_SOURCEFILES += code.c fft.c gf2x.c gf.c hqc.c kem.c parsing.c
PROJECT_SOURCEFILES += reed_muller.c reed_solomon.c shake_ds.c shake_prng.c vector.c
PROJECT_SOURCEFILES += fips202.c aes.c fragmentation.c crypto_worker.c handshake_fsm.c
PROJECT_SOURCEFILES += session_manager.c PQC.c metrics.c

CFLAGS += -I$(APP_SRC)
CFLAGS += -I$(COMMON_SRC)
CFLAGS += -I$(TEMP_SRC)
CFLAGS += -I$(PQC_SRC)
CFLAGS += -I$(METRICS_SRC)
CFLAGS += -I$(HQC_DIR)
CFLAGS += -I$(HQC_FIPS202_DIR)
CFLAGS += -I$(AES_DIR)
CFLAGS += -Wno-error=array-bounds
CFLAGS += -Wno-array-bounds
CFLAGS += -DSL_STACK_SIZE=65536
CFLAGS += -DSL_HEAP_SIZE=16384

CONTIKI = ../..
include $(CONTIKI)/Makefile.include
