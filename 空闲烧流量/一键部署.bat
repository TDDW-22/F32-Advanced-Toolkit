@echo off
chcp 65001 >nul
setlocal

set DEV=/mnt/userdata/etc_rw
set HERE=%~dp0

where adb >nul 2>nul && (set ADB=adb) || (set ADB=%~dp0..\adb.exe)

echo [1/5] 检查设备连接...
"%ADB%" devices
"%ADB%" shell echo ok >nul 2>nul || (echo 设备未连接，请插 USB 并确认 adb devices & pause & exit /b 1)

echo [2/5] 停止旧实例...
"%ADB%" shell "%DEV%/burnctl stop 2>/dev/null"

echo [3/5] 推送 burnctl...
"%ADB%" shell "rm -f %DEV%/burnctl"
"%ADB%" push "%HERE%burnctl" %DEV%/burnctl || (echo 推送失败 & pause & exit /b 1)
"%ADB%" shell "chmod 755 %DEV%/burnctl"

echo [4/5] 自检（重点看 ICCID 是否匹配、在线终端）...
"%ADB%" shell "%DEV%/burnctl check"

echo.
echo [5/5] 写自启 + 启动...
"%ADB%" shell "mount -o remount,rw /"
"%ADB%" shell "grep -q 'burnctl start' /etc/rc || echo '( sleep 15; %DEV%/burnctl start ) &' >> /etc/rc"
"%ADB%" shell "ln -sf %DEV%/burn_info.txt /etc_ro/web/js/burn_info.txt"
"%ADB%" shell "mount -o remount,ro /"
"%ADB%" shell "%DEV%/burnctl start"
"%ADB%" shell "%DEV%/burnctl status"
"%ADB%" shell "sync; sync"

echo.
echo 完成。用手机连一下 WiFi 再断开，观察事件是否触发：
echo   adb shell "tail -f %DEV%/burn.log"
pause
