@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

set DEV=/mnt/userdata/etc_rw
set HERE=%~dp0

where adb >nul 2>nul && set ADB=adb
if not defined ADB if exist "%~dp0..\adb.exe" set ADB=%~dp0..\adb.exe
if not defined ADB set ADB=adb

rem 生成一次性 token（8 位以上，只含字母数字）
set TOKEN=F32%RANDOM%%RANDOM%a

echo ============================================================
echo  F32 手机运维通道 · 一次性部署（只需电脑跑这一次）
echo ============================================================

echo [1/6] 检查设备...
"%ADB%" devices
"%ADB%" shell echo ok >nul 2>nul || (echo 设备未连接，请插 USB 并确认 adb devices & pause & exit /b 1)

echo [2/6] 备份现有 smsapi...
"%ADB%" shell "cp /sbin/smsapi %DEV%/smsapi.bak_prephone 2>/dev/null && echo 已备份到 smsapi.bak_prephone"

echo [3/6] 推送新 smsapi（含 /api/file 与 /api/exec）...
"%ADB%" push "%~dp0..\短信转发组件\smsapi" /sbin/smsapi.new || (echo 推送失败 & pause & exit /b 1)
"%ADB%" shell "chmod 755 /sbin/smsapi.new && mv /sbin/smsapi.new /sbin/smsapi && echo 已替换"

echo [4/6] 写入 api_key 并重启 smsapi...
"%ADB%" shell "cd %DEV% && (grep -v '^api_key=' smsfwd.conf 2>/dev/null > .k.tmp); echo 'api_key=%TOKEN%' >> .k.tmp; cat .k.tmp > smsfwd.conf; rm -f .k.tmp; echo 配置已写入"
"%ADB%" shell "kill $(cat %DEV%/smsapi.pid 2>/dev/null) 2>/dev/null; sleep 1; /sbin/smsapi; sleep 1; echo -n 'smsapi pid: '; cat %DEV%/smsapi.pid"

echo [5/6] 推送手机运维页到设备 web...
"%ADB%" push "%HERE%m.html" /etc_ro/web/m.html && "%ADB%" shell "chmod 644 /etc_ro/web/m.html && echo 已推送"

echo [6/6] 完成
echo.
echo ------------------------------------------------------------
echo  手机运维 token :  %TOKEN%
echo  手机上打开     :  http://192.168.0.1/m.html
echo  （手机需连着这台设备的 WiFi）
echo ------------------------------------------------------------
echo.
echo 之后所有操作都在手机上完成：选文件上传、点按钮执行命令、看日志。
pause
