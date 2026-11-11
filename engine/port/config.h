// Prefix header of the Lumia port (engine/port), included first by every port source file, like each WebKit
// project's own config.h.
#pragma once

#if defined(HAVE_CONFIG_H) && HAVE_CONFIG_H && defined(BUILDING_WITH_CMAKE)
#include "cmakeconfig.h"
#endif

#include <JavaScriptCore/JSExportMacros.h>
// jsctest is JavaScriptCore alone: no WebCore headers on its include path.
#if __has_include(<WebCore/PlatformExportMacros.h>)
#include <WebCore/PlatformExportMacros.h>
#include <pal/ExportMacros.h>
#endif

#ifdef __cplusplus
#undef new
#undef delete
#include <wtf/FastMalloc.h>
#include <wtf/TZoneMalloc.h>
#endif
