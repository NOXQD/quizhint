#pragma once

// Заголовки, сгенерированные wayland-scanner (см. Makefile).
#include <wayland-client.h>

// В wlr-layer-shell у get_layer_surface есть аргумент «namespace» — это
// ключевое слово C++, поэтому на время включения переименовываем его.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
