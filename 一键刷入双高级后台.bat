@echo off
chcp 65001 >nul
title 中兴F32/F30Bpro 高级后台 + 短信转发到钉钉
color 0a
setlocal

set SCRIPT_DIR=%~dp0
set ADB=%SCRIPT_DIR%adb.exe
set WEB=%SCRIPT_DIR%web
set ATWEB_FILE=%SCRIPT_DIR%atweb
set COMP=%SCRIPT_DIR%短信转发组件
set QUOTA=%SCRIPT_DIR%配额断网组件
set CONF=/mnt/userdata/etc_rw/smsfwd.conf
set QCONF=/mnt/userdata/etc_rw/quota.conf

echo ==========================================
echo  中兴F32/F30Bpro 高级后台 + 短信转发到钉钉
echo ==========================================
echo.
echo 本次安装会做五件事：
echo   1. 替换 /etc_ro/web（网页里的“短信”改成了“短信转发”）
echo   2. 装 atweb 高级后台（9090 端口）
echo   3. 装 smsfwd 转发程序（把设备短信推到钉钉）
echo   4. 装 smsapi 控制服务（8080 端口，网页上查看状态/改参数）
echo   5. 装配额断网组件（首页填套餐总额，已用自动统计，到量自动断网；开机公共 DNS 下发）
echo.
echo 请确保设备已开 ADB 调试并连上电脑。
echo.
pause

echo 正在检查 ADB 连接...
"%ADB%" devices
"%ADB%" get-state >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo ADB 连接失败：请检查 USB 线、驱动、USB 调试是否打开。
    goto END
)

if not exist "%WEB%" (
    echo 错误：同目录下未找到 web 文件夹。
    goto END
)
if not exist "%ATWEB_FILE%" (
    echo 错误：同目录下未找到 atweb 文件。
    goto END
)
if not exist "%COMP%\smsapi" (
    echo 错误：未找到 短信转发组件\smsapi。
    goto END
)
if not exist "%COMP%\smsfwd" (
    echo 错误：未找到 短信转发组件\smsfwd。
    goto END
)
if not exist "%QUOTA%\quota" (
    echo 错误：未找到 配额断网组件\quota。
    goto END
)

echo.
echo [1/10] 挂载系统分区为可读写...
"%ADB%" shell "mount -o remount,rw /"
if %errorlevel% neq 0 (
    echo 挂载失败，可能没有 root 权限。
    goto END
)

echo [2/10] 上传 web 到 /etc_ro/web ...
"%ADB%" shell "rm -rf /etc_ro/web"
"%ADB%" push "%WEB%" /etc_ro/web
"%ADB%" shell "chmod -R 755 /etc_ro/web"
if %errorlevel% neq 0 (
    echo 上传 web 失败。
    goto END
)
echo     重建首页配额数字软链 ...
"%ADB%" shell "test -f /mnt/userdata/etc_rw/quota_info.txt || echo '0.00GB|0.00GB|0.00GB|ok' > /mnt/userdata/etc_rw/quota_info.txt"
"%ADB%" shell "ln -sf /mnt/userdata/etc_rw/quota_info.txt /etc_ro/web/js/quota_info.txt"

echo [3/10] 上传 atweb 到 /sbin ...
    "%ADB%" shell "rm -f /sbin/atweb"
    "%ADB%" push "%ATWEB_FILE%" /sbin/atweb
"%ADB%" shell "chmod 755 /sbin/atweb"

echo [4/10] 上传 smsapi 到 /sbin ...
    "%ADB%" shell "rm -f /sbin/smsapi"
    "%ADB%" push "%COMP%\smsapi" /sbin/smsapi
"%ADB%" shell "chmod 755 /sbin/smsapi"

echo [5/10] 上传 smsfwd 到 /mnt/userdata/etc_rw ...
"%ADB%" push "%COMP%\smsfwd" /mnt/userdata/etc_rw/smsfwd
"%ADB%" shell "chmod 755 /mnt/userdata/etc_rw/smsfwd"
"%ADB%" push "%COMP%\smsfwd_ca.pem" /mnt/userdata/etc_rw/smsfwd_ca.pem

echo [6/10] 检查 smsfwd.conf ...
"%ADB%" shell "test -f %CONF% && echo EXISTS || echo MISSING" > "%TEMP%\smsfwd_check.txt" 2>&1
findstr /C:"MISSING" "%TEMP%\smsfwd_check.txt" >nul
if %errorlevel% equ 0 (
    "%ADB%" push "%COMP%\smsfwd.conf" %CONF%
    "%ADB%" shell "chmod 666 %CONF%"
    echo 已写入空白配置：钉钉 Webhook 留空，刷完请到网页“短信 → 转发设置”填写后保存。
) else (
    echo 设备上已有 smsfwd.conf，保留你当前的 Webhook 与参数不改动。
)

echo [7/10] 上传配额断网组件 ...
"%ADB%" push "%QUOTA%\quota" /mnt/userdata/etc_rw/quota
"%ADB%" shell "chmod 755 /mnt/userdata/etc_rw/quota"
"%ADB%" push "%QUOTA%\fwguard.sh" /mnt/userdata/etc_rw/fwguard.sh
"%ADB%" push "%QUOTA%\netfix.sh" /mnt/userdata/etc_rw/netfix.sh
"%ADB%" shell "chmod 755 /mnt/userdata/etc_rw/fwguard.sh /mnt/userdata/etc_rw/netfix.sh"
"%ADB%" shell "test -f %QCONF% && echo EXISTS || echo MISSING" > "%TEMP%\quota_check.txt" 2>&1
findstr /C:"MISSING" "%TEMP%\quota_check.txt" >nul
if %errorlevel% equ 0 (
    "%ADB%" push "%QUOTA%\quota.conf" %QCONF%
    "%ADB%" shell "chmod 666 %QCONF%"
    echo 已写入默认配额（总额 0=不限量），刷完到首页填写套餐总额（已用自动统计）。
) else (
    echo 设备上已有 quota.conf，保留你当前的套餐总额不改动。
)


echo [8/10] 写入开机自启 ...
"%ADB%" shell "grep -q 'smsapi.pid' /etc/rc || echo 'rm -f /mnt/userdata/etc_rw/smsapi.pid /mnt/userdata/etc_rw/smsfwd.pid' >> /etc/rc"
    "%ADB%" shell "grep -q 'sleep 20' /etc/rc || echo 'sleep 20' >> /etc/rc"
"%ADB%" shell "grep -q '/sbin/atweb' /etc/rc || echo '/sbin/atweb &' >> /etc/rc"
"%ADB%" shell "grep -q '/sbin/smsapi' /etc/rc || echo '/sbin/smsapi' >> /etc/rc"
"%ADB%" shell "grep -q 'smsfwd.conf start' /etc/rc || echo '( sleep 10; /mnt/userdata/etc_rw/smsfwd /mnt/userdata/etc_rw/smsfwd.conf start ) &' >> /etc/rc"
"%ADB%" shell "grep -q 'fwguard.sh' /etc/rc || echo '( sleep 5; sh /mnt/userdata/etc_rw/fwguard.sh ) &' >> /etc/rc"
"%ADB%" shell "grep -q 'quota daemon' /etc/rc || echo '( sleep 60; /mnt/userdata/etc_rw/quota daemon ) &' >> /etc/rc"

echo [9/10] 立即启动服务 ...
"%ADB%" shell "kill $(cat /mnt/userdata/etc_rw/smsapi.pid 2>/dev/null) 2>/dev/null; rm -f /mnt/userdata/etc_rw/smsapi.pid"
"%ADB%" shell "/sbin/smsapi"
"%ADB%" shell "/mnt/userdata/etc_rw/smsfwd %CONF% start"
"%ADB%" shell "sh /mnt/userdata/etc_rw/fwguard.sh"
"%ADB%" shell "/mnt/userdata/etc_rw/quota daemon"

echo [10/10] 校验并落盘 ...
"%ADB%" shell "netstat -ltn | grep 8080"
"%ADB%" shell "cat /mnt/userdata/etc_rw/smsfwd.pid"
echo 同步并强制提交到 flash（不这样做的话异常重启会回滚已推送的文件）...
"%ADB%" shell "sync; sync"
"%ADB%" shell "mount -o remount,ro /; mount -o remount,rw /"
echo 校验关键文件大小（不一致说明上传被中断，重跑一次本脚本）...
call :CHK "js\sms\smslist.js" "/etc_ro/web/js/sms/smslist.js"
call :CHK "tmpl\sms\smslist.html" "/etc_ro/web/tmpl/sms/smslist.html"
call :CHK "tmpl\sms\sms_setting.html" "/etc_ro/web/tmpl/sms/sms_setting.html"
call :CHK "js\config\ufi\menu.js" "/etc_ro/web/js/config/ufi/menu.js"
call :CHK "i18n\Messages_zh-cn.properties" "/etc_ro/web/i18n/Messages_zh-cn.properties"
call :CHK "js\home.js" "/etc_ro/web/js/home.js"
call :CHK "tmpl\home.html" "/etc_ro/web/tmpl/home.html"
"%ADB%" shell "ls -l /mnt/userdata/etc_rw/quota" | findstr /C:"70852" >nul && echo   OK   /mnt/userdata/etc_rw/quota  (70852 字节) || echo   [警告] quota 大小不一致，请重跑一次本脚本

echo.
echo ==========================================
echo  安装完成
echo   网页后台 : http://192.168.0.1/       （左侧“短信”= 短信转发）
echo   高级后台 : http://192.168.0.1:9090/
echo   转发状态 : 打开网页“短信”→“转发记录”查看，
echo              点“测试推送”可立刻发一条测试消息到钉钉。
echo   配额断网 : 首页橙色流量卡片填套餐总额后保存（已用自动统计），用到总额自动断网；
echo              总额留 0 表示不限量。
echo 提示：包内钉钉 Webhook 已清空（不含任何 token），首次使用请先到网页“转发设置”填写。
echo ==========================================
echo.
set /p confirm=是否立即重启设备？(y/n):
if /i "%confirm%"=="y" (
    "%ADB%" shell "sync; sync"
    "%ADB%" shell reboot
)

"%ADB%" kill-server 2>nul
pause
goto END

:CHK
for %%F in ("%WEB%\%~1") do set SZ=%%~zF
"%ADB%" shell "ls -l %~2" > "%TEMP%\chk_size.txt" 2>&1
findstr /C:"%SZ%" "%TEMP%\chk_size.txt" >nul
if %errorlevel% equ 0 echo   OK   %~2  (%SZ% 字节)
if %errorlevel% neq 0 echo   [警告] %~2 大小不一致，本地应为 %SZ% 字节，请重跑一次本脚本
goto :eof

:END
pause
