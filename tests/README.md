# tests/

每个子目录是一个独立可执行测试,`qmake && make` 后直接运行,退出码非零即失败。
CI 统一经 `scripts/run-test.bat`(Windows)调用,法规约见脚本头注释;新测试加入
CI 时在 build.yml 对应步骤加一行 `call scripts\run-test.bat <目录> <TARGET>`。

断言风格尚未统一(require 累积 / CHECK 返回行号 / qFatal 混用),新测试建议用
`require(bool, QString)` 累积风格(clipboard_payload_routing 是范本),保持
main() 短、被测逻辑走生产源文件直编(见各 .pro 的 SOURCES)。

## CI 覆盖(.github/workflows/build.yml)

| 目录 | 验证内容 | CI 平台 |
|---|---|---|
| derive_version | scripts/derive-version.py 版本号推导 | Ubuntu(unittest) |
| cursor_shape_classification | 光标位图 → 形状分类器的判定矩阵 | Windows |
| clipboard_payload_routing | 剪贴板状态、复合内容、文件保护、文本/图片同步开关、主机文件推送(解析/文件名清洗/下载/超时)、拖放上传(目录展开/分块/偏移重同步/取消/超时)、文件复制粘贴(回声抑制/新复制优先/持久目录清理)、IPC 分帧和本地 TLS 传输回归（offscreen） | Windows |
| clipboard_helper_lifecycle | helper 异常退出、响应超时和进程管道背压 | Windows |
| clipboard_engine_thread | 主机→客户端文字写入真实系统剪贴板（helper 引擎线程模型；原生平台，测试前后保存/恢复剪贴板） | Windows |
| overlay_button_position | 悬浮按钮归一化/还原坐标的跨分辨率往返 | Windows |
| pyrowave_color | PyroWave 颜色合同表（SDR/HDR10/HLG × 范围）、HDR 平面位编码、SS_HDR_METADATA 解析与快照保留规则、libplacebo 元数据映射 | Windows |
| pyrowave_pacing | PyroWave 呈现策略：延迟呈现后切换三重缓冲的阈值、串流快于显示器时的帧节奏抽帧 | Windows |
| present_stall | Pacer 一次性卡顿检测：有效呈现帧率、连续慢帧重置和 500ms 丢帧窗口 | Windows |
| dxgi_present_queue | D3D11 呈现队列模拟:DWM 合成时帧延迟 1 + 可等待对象 + 只渲染最新帧,复现 r33 现场数据 | Windows |
| vrr_nonary_controller | Nonary VRR 节奏器控制器（移植自 Nonary/moonlight-qt@c822745 的 tst_vrrtimingcontroller）：时间戳排程、播出延迟、掉帧与同步翻转保护 | Windows |
| file_mapping_websocket_framing | 文件映射 WS 帧解析(fin/分片/ping-pong) | Windows |
| shared_helpers | 配对证书策略、版本比较与 GFE 兼容性回退 | Windows |
| amf_lifecycle | AMF 会话探测、初始化/重配/帧处理失败、析构与多会话的对象/DLL 释放顺序 | Windows |
| file_mapping_transport | 本地 TLS 配对/缓存连接身份/重定向校验，WS Upgrade 与 RPC ID/重入回归 | Windows |
| file_mapping_mirror_e2e | 主机文件镜像端到端(FakeRemoteVfs + 挂载提供方) | Windows |
| usb_forwarding_capability | helper 能力 JSON 解析(端口/token/畸形输入) | Windows |
| usb_forwarding_environment | USB 转发就绪态 → 错误文案映射(--require-ready 走活探测) | Windows |
| usb_forwarding_backend_list | moonlight-usbd `list --json` → 设备表解析 | Windows |
| usb_forwarding_backend_sysfs | Linux sysfs 枚举 → 设备表解析（夹具目录，跳过 hub/接口/根 hub） | Linux |
| ds5_ir_renderer | DualSense IR 渲染、PCM 边界和控制报文 | Windows |
| ds5_haptics_worker | 生产发送线程的状态顺序、阻塞写入、设备切换、停止屏障和短效果排空 | Windows |
| keyboard_state | 扩展键区分、国际键标志、失焦松键失败重试 | Windows |
| system_key_capture | Production capture policy and menu-toggle lifecycle with fake native input | Ubuntu(unittest + C++) |
| resolution_match | 客户端驱动分辨率:Session 方法 + 源码门禁(python unittest,需 C++ 编译器) | Ubuntu(unittest + C++) |
| gamepad_sensor_rate | 低传感器频率周期存储、禁用值、全频率范围限速上界 | Windows |
| pen_history_selection | 手写笔历史选择 | Windows |
| stylus_replay | 手写笔录制回放 | Windows |
| overlay_event_wake_state | 悬浮菜单事件唤醒状态 | Windows |
| overlay_button_native_wake | 悬浮按钮原生唤醒 | Windows + macOS + Linux(xvfb) |
| overlay_toast_event_state | 悬浮 toast 事件状态 | Windows |
| stats_overlay_layout | Production stats painter/layout and move-mode controls, compiled zh_CN/zh_TW labels, real controller preset/detach/Off-preview lifecycle with a fake connection/renderer (offscreen) | Windows |
| overlay_menu_navigation | 悬浮菜单键盘/鼠标导航(QTest 事件,需 offscreen) | Windows |
| stream_quality | 串流画质菜单的分辨率/帧率预设生成、配置差异与切换路径判定 | Windows |
| app_picker | 名称/类型/运行状态/隐藏状态筛选，排序与分组顺序 | Windows |
| settings_localization | 中文翻译 .qm + 设置页布局(qmltestrunner) | Windows |
| settings_search | 设置搜索：排序/归一化，以及在真实 SettingsRow/Card/ScrollArea 搭的壳上验证分类切换、滚动入视野、焦点落到控件、高亮、Esc/手柄键（qmltestrunner + `-import tests\settings_search\mocks`）；`test_*.py` 是元数据门禁（每个设置有标题、能解析到分类、用 applicable 而非 visible） | Windows（QML）+ Ubuntu（unittest） |
| linux_display_event_monitor | Linux 显示事件唤醒 | Linux |

## 仅本地/手动

| 目录 | 说明 |
|---|---|
| usb_forwarding_tunnel | USB/IP 隧道手动联调探针:需要 10 个 CLI 参数和真实 usbipd 服务端,不能进 CI。用法看 main.cpp 头注释 |
| shortcuts | 键盘快捷键:绑定解析/格式化、键位转换、默认前缀、冲突、持久化、匹配器。纯 QtCore,可直接进 CI(尚未在 build.yml 登记) |
| stats_model | 串流统计模型:百分位/1% 与 0.1% 低帧、呈现时间环、会话直方图、RFC 3550 抖动、延迟分解、快照与会话累计。纯 QtCore,可直接进 CI(尚未在 build.yml 登记) |
| super_cmds | Foundation Sunshine 超级指令:`<SuperCmds>` JSON 解析(null/空/畸形/XML 转义)、NvApp 设置缓存往返。纯 QtCore,可直接进 CI(尚未在 build.yml 登记) |
| host_keys | 发送到主机的组合键:扫描码→VK 映射(与 keyboard.cpp 一致)、按下/松开序列、特殊键、自定义宏的校验与 QSettings 持久化。纯 QtCore,可直接进 CI(尚未在 build.yml 登记) |
| vrr_nonary_worker | Nonary VRR 工作线程测试（移植自 Nonary 的 tst_vrrpacingworker）。部分用例依赖真实时钟：机器繁忙时 Nonary 原版同样会偶发失败，所以不进 CI |
| vrr_nonary_sim | Nonary 的 vrrqueuesim（加了 min/stddev 与“added jerk”输出）。`canonical_scenarios.cpp` 按 tests/vrr_policy 的 generateArrivals 生成同一组到达序列的 CSV，用于和分叉自带的 VRR 节奏器对比 |
| `file-mapping/smoke/` | 文件映射冒烟二进制,复用 app 源码,手工构建运行。刻意**不注册**进 moonlight-qt.pro 的 SUBDIRS:它链接 app 后端与 OpenSSL,注册会给全部平台构建(含 SteamLink 老工具链)平添失败面,而它没有自动化验证价值 |

## 约定

The VRR mode matrix also compiles Session's production decoder-parameter setup:
Smooth is opt-in and only applies to the fork pacer, never Nonary, its fallback,
VRR-off or decoder probes. `settings_search/tst_vrr_modes.qml` instantiates the
actual Display settings toggle with a fake preference backend and checks that
Nonary/VRR-off disable it without overwriting its saved value. Run QML search
tests with `QML_XHR_ALLOW_FILE_READ=1` so the fixture can read that production row.

`derive_version` round-trips the CI-frozen artifact string (including the r25/r33
regression fixtures) back to its numeric version. Development EXE, MSI and setup
versions retain the commit count as `major.minor.patch.count`; exact tags remain
`.0`. Releases must advance the three-field base (or supply a higher explicit
four-part `CI_VERSION`) rather than reset the revision below a published build.
`scripts/verify-windows-package.ps1` checks the built EXE/MSI/setup versions and
the compiled MSI Upgrade/InstallExecuteSequence tables before packaging succeeds.

MSI compares only the first three version fields, so `AllowSameVersionUpgrades`
is required even though file replacement and the setup bundle use all four.
Install through the setup EXE: direct MSI installs can downgrade revisions with
the same three-field base. The hosted-Windows `test_windows_upgrade.ps1 -Install`
gate builds predecessor bundles with inert payloads at revision 0 and N-1, then
upgrades each through the actual new setup. It checks the installed EXE hash,
old-product removal, obsolete-file removal and preservation of user settings.
Without `-Install` it only builds fixtures; installation is refused outside
disposable GitHub-hosted runners. It never launches Moonlight.

`stats_overlay_layout` also exercises the production overlay painter at 75/80/100% and
1/1.5/2 device-pixel ratios, verifies readable Compact labels, all 13 metrics, flush-top
placement/editing, 55% metric chips with transparent gaps and soft shadows,
generous resize zones, exact free dragging with 4 px magnets, conditional edit-on-click,
and preserves explicit settings when defaults change. Set
`MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR` to save PNGs for visual inspection; it does not start
a stream or send desktop input.

`shortcuts` covers the Right Alt checkbox, legacy bare-Right-Alt opt-in, the existing
matcher's AltGr suppression, and deterministic hold/double-tap/Esc/focus behavior.
`overlay_menu_navigation` also tests persistent-release editing: clicks do not fire
actions, the body drags, edges resize with fixed opposite edges, wheel zooms and
Ctrl+wheel changes opacity through the persistence callback. These use only their
own offscreen windows; OS-level capture and real-stream key delivery still require
verification on the receiving client.

`overlay_menu_navigation` enforces shortcut-registry/menu parity with only the
menu-toggle and physical Right Alt hold excluded. It checks actual row invocation,
payload-specific Ctrl+Alt+Del hints, live rebinding and removal, retained gamepad
and remote-chord details, and the input matcher's held-key release across a rebind.
The same profile snapshot updates hints and dispatch while the menu is open.
Cancel Sending Files (Host tab) stays greyed out and inert until an upload is
pending or running, enables live while the menu is open, and shows its rebound shortcut.

`system_key_capture` compiles the actual input capture, Right Alt gesture and
Session geometry methods against fake SDL/Qt windows. It preserves the existing
capture modes, checks the menu-toggle lifecycle, fullscreen/borderless transitions,
one-pixel gaps, negative monitor origins, separate Qt logical/SDL geometry,
Right Alt hold/latch/Esc, and failed display queries. Its compatibility gate pins
all existing `StreamingPreferences` Q_ENUM integers, including nonzero/negative
starts; reordering a persisted enum fails CI. Run `python -m unittest discover -s
tests/system_key_capture -p 'test_*.py'` from a C++ compiler environment.

The appended mode (`CSK_FULLSCREEN_OR_BORDERLESS = 3`, CLI
`--capture-system-keys fullscreen-or-borderless`) adds frameless windows in the
Borderless Window display mode whose client rectangle covers their whole monitor.
Ordinary/maximized decorated windows and partial coverage do not qualify. Existing
Fullscreen mode retains its previous semantics, including SDL desktop fullscreen.
The mode never overrides temporary or persistent Right Alt release.
`settings_search` also checks the new long choice label and value-3 selection in
the real ChoiceRow at narrow/wide widths. Its fixture values are checked against
the production model. Run the Windows QML tests with
`QT_QUICK_CONTROLS_STYLE=FluentWinUI3`, matching the app rather than the test runner's
unsupported native customization style. These tests use offscreen windows/fakes;
they do not capture desktop input or verify delivery on a real receiving client.

`resolution_match` compiles the production Session resize/report methods with fake
time, window geometry, transport and decoder-reset events. Run `python -m unittest
discover -s tests/resolution_match -p 'test_*.py'` from a C++ compiler environment
(MSVC developer shell on Windows). It covers disabled/unsupported hosts sending
nothing, debounce, one outstanding request, timeout, 1:1 display gating, fullscreen,
decoder reset failure and host-initiated changes with matching disabled. Source
integration gates also protect unique SDL event codes, connection-state reset,
active-size menu comparisons and explicit quality choices.

Window matching defaults off. It needs a Foundation Sunshine build containing
[AlkaidLab/foundation-sunshine#729](https://github.com/AlkaidLab/foundation-sunshine/pull/729)
with `allow_client_resolution_change` enabled; normal releases without its negotiated
`0x400` capability stay inert. These tests do not alter a host or its display.

- 测试**不得**把 Makefile/.o/可执行文件提交进仓库(.gitignore 已覆盖
  `tests/*/{Makefile*,.qmake.stash,*.o,release,debug}`),历史上曾误提交过
  Linux 二进制毒化 Windows 构建(.qmake.stash 被 MSVC qmake 加载直接报错)。
- 被测生产代码有平台分支的(如 clipboardsync.cpp 的 macx 段),跨平台跑法在
  .pro 里用条件 SOURCES 处理,别在测试里复制逻辑。

AMF 生命周期测试直接编译生产 `AmfSession`，通过实现 SDK C ABI 的测试 DLL 注入初始化、
重配和帧处理失败，验证对象释放顺序、空返回、重复清理和多会话 DLL 引用，不访问会话私有字段。
装有支持视频超分的 AMD 驱动时，
可执行 `scripts\run-test.bat amf_lifecycle amf_lifecycle --hardware`，额外验证真实 D3D11/AMF
的 NV12/P010 放大/缩小、锐度重配、帧拷贝、重配失败清理和析构；此硬件选项不在 CI 中运行。
独立搭建 VP 管线的[诊断工具](https://github.com/qiin2333/moonlight-qt/tree/22bdda430ac75060282386e9c59bb57aed910ca8/diagnostics/d3d11-vp)
已拆到单独分支，只检查驱动资源组合，不作为生产渲染器回退路径的回归证明。

剪贴板 TLS 回归仅访问本机随机端口。`clipboard_payload_routing/fixtures/` 中的证书和私钥是公开测试素材，不能用于实际配对；证书有效期至 2036 年，届时需要更新。测试不会访问用户配对凭据，也不使用系统剪贴板。macOS 的原生复合写入路径仍需实机验证。
