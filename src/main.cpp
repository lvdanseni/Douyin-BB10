#include <bb/cascades/Application>
#include <stdlib.h>

#include "applicationui.hpp"

using namespace bb::cascades;

Q_DECL_EXPORT int main(int argc, char** argv)
{
    // ========================================================================
    // 【v50 关键修复】必须在任何库加载之前执行 —— 关掉一切代理
    // ========================================================================
    // 真机 v49 的 libbbplayer 事件回调给出了铁证：
    //   ERROR: cannot read https://upos-sz-estgcos.bilivideo.com/...mp4:
    //          Could not resolve proxy name
    //
    // "Could not resolve proxy name" 是 curl 的 CURLE_COULDNT_RESOLVE_PROXY，
    // 即"代理主机名解析失败"。逆向 libbbnet.so 找到了来源：
    //   bbnet_setopt_default_proxy(0x35b74) 的逻辑是
    //     if (全局代理串 == NULL || *全局代理串 == 0)      // 没配代理 / 配了空串
    //         curl_easy_setopt(handle, CURLOPT_PROXY, <写死的默认代理串>);
    //     else
    //         用调用方配置的代理（proxy / user / pass）
    // 我们一直传的是空串（setNetConfig 第三个参数），于是走进了"默认代理"分支，
    // 那个默认串在本机解析不了 → 每个 HTTP 请求立刻失败 → 打开视频必报错 →
    // position/duration 永远 0、state 卡在 5(ERROR)。这就是 v43~v49 黑屏的真因。
    //
    // 对策：no_proxy=* 是 curl 与 FFmpeg 共同支持的语义，且 curl 文档明确写：
    //   "This string is a list of host names that should not go through a proxy
    //    even if it is specified in CURLOPT_PROXY."
    // 也就是说即使库已经把 CURLOPT_PROXY 设成那个默认代理，no_proxy=* 仍会让它被绕过。
    //
    // 另外把 http_proxy / all_proxy 等环境变量清掉：FFmpeg 的 http 协议会
    // 读 http_proxy 环境变量，若值是空串会被当成"空主机名代理"，反而更糟。
    // ========================================================================
    ::setenv("no_proxy", "*", 1);
    ::setenv("NO_PROXY", "*", 1);
    ::unsetenv("http_proxy");
    ::unsetenv("HTTP_PROXY");
    ::unsetenv("https_proxy");
    ::unsetenv("HTTPS_PROXY");
    ::unsetenv("all_proxy");
    ::unsetenv("ALL_PROXY");

    Application app(argc, argv);
    ApplicationUI ui;
    return Application::exec();
}
