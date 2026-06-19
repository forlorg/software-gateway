#pragma once

/**
 * @file web_assets.h
 * @brief HTTP 页面静态资源，编译进 PROGMEM 以便独立维护页面内容。
 */

namespace gateway::web_assets {

    extern const char kIndexHtml[];
    extern const char kStyleCss[];
    extern const char kAppJs[];

} // namespace gateway::web_assets
