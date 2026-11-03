ifeq ($(origin CC),default)
CC := gcc
endif
VERSION    ?= dev
VULKAN_INC ?=
PYTHON     ?= python3
OUT        := build
DLL        := $(OUT)/VK_LAYER_MHGU_fsr4.dll
STAGE      := dist/stage
SMOKE      := $(OUT)/smoke

ifeq ($(OS),Windows_NT)
WINE ?=
else
WINE ?= wine64
endif

INC      := -Isrc -Iinclude -Ivendor/include -Ivendor/minhook -Ivendor/ffx-api/api/include -Ivendor/ffx-api/upscalers/include $(VULKAN_INC)
CFLAGS   ?= -O2
CFLAGS   += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type \
            -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DPLUGIN_VERSION='"$(VERSION)"' -MMD -MP $(INC)
LDFLAGS  += -shared -static-libgcc -s
LIBS     := -lversion

SRC      := $(wildcard src/*.c)
MINHOOK  := vendor/minhook/hook.c vendor/minhook/buffer.c vendor/minhook/trampoline.c vendor/minhook/hde/hde64.c
OBJ      := $(patsubst %.c,$(OUT)/%.o,$(SRC) $(MINHOOK))

all: $(DLL)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/vendor/minhook/%.o: CFLAGS += -w
$(OUT)/vendor/minhook/hde/%.o: CFLAGS += -w

$(DLL): $(OBJ) src/layer.def
	$(CC) $(LDFLAGS) -o $@ $(OBJ) src/layer.def $(LIBS)

smoke: $(DLL)
	@mkdir -p $(SMOKE)
	$(CC) $(CFLAGS) -O0 -w -o $(SMOKE)/mhgurecomp.exe tests/smoke/smoke_host.c
	$(CC) $(CFLAGS) -w -shared -o $(SMOKE)/amd_fidelityfx_loader_vk.dll tests/smoke/fake_loader.c
	$(CC) $(CFLAGS) -w -shared -o $(SMOKE)/vulkan-1.dll tests/smoke/fake_vulkan.c
	cp $(SMOKE)/vulkan-1.dll $(SMOKE)/amd_fidelityfx_upscaler_vk.dll
	cp $(DLL) $(SMOKE)/
	WINE="$(WINE)" sh tests/run_smoke.sh $(SMOKE)

test-coff: tests/test_coff.c src/coff.c
	@mkdir -p $(OUT)
	gcc -O1 -Wall -o $(OUT)/test_coff tests/test_coff.c src/coff.c
	$(OUT)/test_coff "$(EXE)" ffxVkPortableUpscaleContextRecordDispatch ffxVkPortableUpscaleContextCreate

dist: $(DLL)
	rm -rf $(STAGE)
	mkdir -p $(STAGE)/fsr4plugin
	cp $(DLL) package/VK_LAYER_MHGU_fsr4.json package/README.txt LICENSE THIRD_PARTY_NOTICES.md $(STAGE)/fsr4plugin/
	cp package/Start-MHGURecomp-FSR4.bat $(STAGE)/

package: dist
	$(PYTHON) tools/package.py $(STAGE) dist/MHGURecomp-FSR4-Layer-$(VERSION).zip

clean:
	rm -rf $(OUT) dist

-include $(OBJ:.o=.d)

.PHONY: all smoke test-coff dist package clean
