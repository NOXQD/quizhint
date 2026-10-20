# quizhint — снимок экрана → vision-модель → номер ответа в углу экрана.

PREFIX ?= $(HOME)/.local
BUILD  ?= build

CXX        ?= g++
CC         ?= cc
PKG_CONFIG ?= pkg-config

PKGS := wayland-client cairo libcurl libturbojpeg

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER   := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

# xdg-shell и ext-foreign-toplevel-list нужны только для линковки: на них
# ссылаются layer-shell и ext-image-capture-source.
PROTOCOL_XML := \
	$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml \
	$(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml \
	$(WAYLAND_PROTOCOLS)/staging/fractional-scale/fractional-scale-v1.xml \
	$(WAYLAND_PROTOCOLS)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
	$(WAYLAND_PROTOCOLS)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml \
	$(WAYLAND_PROTOCOLS)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
	protocols/wlr-layer-shell-unstable-v1.xml \
	protocols/wlr-screencopy-unstable-v1.xml

PROTOCOLS := $(basename $(notdir $(PROTOCOL_XML)))
PROTO_HDR := $(PROTOCOLS:%=$(BUILD)/protocols/%-client-protocol.h)
PROTO_OBJ := $(PROTOCOLS:%=$(BUILD)/protocols/%-protocol.o)

vpath %.xml $(sort $(dir $(PROTOCOL_XML)))

CXXFLAGS ?= -O2 -g
CFLAGS   ?= -O2 -g
override CXXFLAGS += -std=c++20 -Wall -Wextra -I$(BUILD)/protocols $(shell $(PKG_CONFIG) --cflags $(PKGS))
override CFLAGS   += $(shell $(PKG_CONFIG) --cflags wayland-client)
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:src/%.cpp=$(BUILD)/%.o) $(BUILD)/config_example.o

PLUGIN_ID    := noxqd.quizhint
PLUGIN_DIR   ?= $(HOME)/.config/omarchy/plugins/$(PLUGIN_ID)
PLUGIN_FILES := $(wildcard omarchy/*)

all: quizhint

quizhint: $(OBJS) $(PROTO_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.cpp $(PROTO_HDR)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# config.example встраивается в программу: из него создаётся конфиг при первой записи.
$(BUILD)/config_example.cpp: config.example
	@mkdir -p $(@D)
	{ printf 'extern const char kConfigExample[];\nconst char kConfigExample[] = R"QHCFG('; \
	  cat $<; printf ')QHCFG";\n'; } > $@

$(BUILD)/config_example.o: $(BUILD)/config_example.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/protocols/%-client-protocol.h: %.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@

$(BUILD)/protocols/%-protocol.c: %.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD)/protocols/%-protocol.o: $(BUILD)/protocols/%-protocol.c
	$(CC) $(CFLAGS) -c $< -o $@

install: quizhint
	install -Dm755 quizhint $(DESTDIR)$(PREFIX)/bin/quizhint

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/quizhint

# Плагин для панели Omarchy: копируется в ~/.config/omarchy/plugins/, оболочка
# подхватывает изменения сама. Включается один раз: omarchy plugin enable noxqd.quizhint
install-plugin:
	install -d $(PLUGIN_DIR)
	install -m644 $(PLUGIN_FILES) $(PLUGIN_DIR)/
	-omarchy-shell shell rescanPlugins >/dev/null 2>&1

uninstall-plugin:
	-omarchy plugin disable $(PLUGIN_ID) >/dev/null 2>&1
	rm -f $(addprefix $(PLUGIN_DIR)/,$(notdir $(PLUGIN_FILES)))
	-rmdir $(PLUGIN_DIR)

clean:
	rm -rf $(BUILD) quizhint

.PHONY: all install uninstall install-plugin uninstall-plugin clean
.SECONDARY:

-include $(OBJS:.o=.d)
