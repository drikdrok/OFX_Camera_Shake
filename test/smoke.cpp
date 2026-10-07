// Smoke test: loads the built .ofx and checks it advertises exactly one
// image-effect plug-in with the expected identifier and API version.
// This catches link errors, missing runtime dependencies and bad exports
// without needing a full OFX host.

#include "ofxCore.h"
#include "ofxImageEffect.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

typedef int (*GetNumberOfPluginsFn)(void);
typedef OfxPlugin* (*GetPluginFn)(int);

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: smoke <path-to-plugin.ofx>\n");
    return 2;
  }
  const char* path = argv[1];

  HMODULE lib = LoadLibraryA(path);
  if (!lib) {
    std::fprintf(stderr, "FAIL: LoadLibrary('%s') failed, GetLastError=%lu\n", path,
                 GetLastError());
    return 1;
  }

  GetNumberOfPluginsFn getCount =
      (GetNumberOfPluginsFn)GetProcAddress(lib, "OfxGetNumberOfPlugins");
  GetPluginFn getPlugin = (GetPluginFn)GetProcAddress(lib, "OfxGetPlugin");
  if (!getCount || !getPlugin) {
    std::fprintf(stderr, "FAIL: missing OfxGetNumberOfPlugins / OfxGetPlugin export\n");
    return 1;
  }

  const int n = getCount();
  std::printf("  plugins advertised: %d\n", n);
  if (n != 1) {
    std::fprintf(stderr, "FAIL: expected 1 plug-in, got %d\n", n);
    return 1;
  }

  OfxPlugin* p = getPlugin(0);
  if (!p) {
    std::fprintf(stderr, "FAIL: OfxGetPlugin(0) returned null\n");
    return 1;
  }

  std::printf("  api            : %s v%d\n", p->pluginApi, p->apiVersion);
  std::printf("  identifier     : %s\n", p->pluginIdentifier);
  std::printf("  version        : %u.%u\n", p->pluginVersionMajor, p->pluginVersionMinor);

  if (std::strcmp(p->pluginApi, kOfxImageEffectPluginApi) != 0) {
    std::fprintf(stderr, "FAIL: unexpected API '%s'\n", p->pluginApi);
    return 1;
  }
  if (std::strcmp(p->pluginIdentifier, "net.drikdrok.CameraShake") != 0) {
    std::fprintf(stderr, "FAIL: unexpected identifier '%s'\n", p->pluginIdentifier);
    return 1;
  }
  if (!p->setHost || !p->mainEntry) {
    std::fprintf(stderr, "FAIL: setHost/mainEntry not populated\n");
    return 1;
  }

  std::printf("  smoke test OK\n");
  return 0;
}
