
![](https://github.com/metalbug/SlimBar11/blob/main/2026-2-15%209-39-14.png)

![](https://github.com/metalbug/SlimBar11/blob/main/SlimBar11.gif)

# SlimBar11

This small utility has only one function: to shrink the Windows 11 taskbar back to small icons.

* **Installation & Usage**: Extract the software to any directory of your choice. It is highly recommended to run it with **Administrator privileges**.
* **Auto-Start on Boot**: If run successfully, the software will automatically add itself to the Task Scheduler to launch silently on startup.
* **Command Line Arguments**:
  * `SlimBar11 -t`: Dock the taskbar to the top of the screen.
  * `SlimBar11 -u`: Disable and uninstall the utility and its auto-start task.
* **Feedback**: If you encounter any issues, please actively provide feedback.

---

* **软件功能**：把 Win11 任务栏变回小图标，并支持自动跟随系统缩放比例。
* **安装运行**：将软件解压到任何你喜欢的目录，请使用**管理员权限**运行。
* **开机自启**：如果软件成功运行，它会自动被添加到“任务计划程序”里，跟随系统开机自动启动。
* **软件原理**：需要向系统任务栏进程注入 DLL 来修改 UI 渲染逻辑。
* **异常排查**：如果运行后任务栏无变化，请检查火绒、360 或 Windows Defender 的防护日志，确认是否被静默拦截。
* **解决方式**：将解压目录或 slimbar11.exe 与 slimbar11_hook.dll 添加至杀软白名单，如有顾虑请克隆源码自行编译。
* **运行参数**：
  * `SlimBar11 -t`：让任务栏置顶显示。
  * `SlimBar11 -u`：停用并卸载该软件及自启任务。
* **问题反馈**：如果遇到任何问题，请积极反馈。
