// Export attributes used by GoldHEN plugins.
#pragma once

#define attr_public        __attribute__((visibility("default")))
#define attr_module_hidden __attribute__((weak)) __attribute__((visibility("hidden")))
