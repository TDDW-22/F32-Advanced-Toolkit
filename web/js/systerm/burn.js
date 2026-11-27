define(["jquery"],function($){
    var iv=null, av=null, auth=null;
    function fmt(n){
        n=parseInt(n,10);
        if(!isFinite(n)||n<=0){return "0 MB";}
        var mb=n/1048576;
        if(mb<1024){return mb.toFixed(2)+" MB";}
        return (mb/1024).toFixed(2)+" GB";
    }
    function alive(){return !!document.getElementById("burnState");}
    function api(){
        var h=location.hostname||"192.168.0.1";
        return "http://"+h+":8080/api/burn";
    }
    function setMsg(t, cls){
        $("#burnAuthMsg").text(t||"").removeClass("ok err");
        if(cls){$("#burnAuthMsg").addClass(cls);}
    }
    function setBusy(b){
        $("#burnAuthBtn,#burnStopBtn").prop("disabled",!!b);
    }
    function renderAuth(){
        if(!auth){return;}
        var cur=(auth.current||"").toUpperCase().replace(/[^0-9A-F]/g,"");
        var prefix=cur.length>=18&&cur.length<=20?cur.slice(0,-4):"";
        $("#burnCurrentIccid").text(auth.current||"读取失败");
        $("#burnSavedIccid").text(auth.manual||"未授权");
        $("#burnImsi").text(auth.manual||"未授权");
        $("#burnIccidPrefix").val(prefix);
    }
    function loadAuth(showMsg){
        if(!alive()){return;}
        $.ajax({url:api()+"?r="+(new Date().getTime()),dataType:"json",cache:false,timeout:12000})
        .done(function(r){
            auth=r||null;
            renderAuth();
            if(showMsg){
                if(r&&r.current&&r.manual&&r.match){setMsg("当前已授权，ICCID 与设备检测值一致。","ok");}
                else if(r&&!r.probe_ok){setMsg("读取设备 ICCID 失败，请稍后刷新。","err");}
            }
            load();
        })
        .fail(function(){
            $("#burnCurrentIccid").text("接口未响应");
            $("#burnIccidPrefix").val("");
            if(showMsg){setMsg("烧流授权接口未响应，请检查 smsapi 是否运行。","err");}
        });
    }
    function submitAuth(){
        var prefix=($("#burnIccidPrefix").val()||"").toUpperCase().replace(/[^0-9A-F]/g,"");
        var suffix=($("#burnIccidInput").val()||"").toUpperCase().replace(/[^0-9A-F]/g,"");
        if(prefix.length<14||prefix.length>16){
            setMsg("设备 ICCID 前段未读取，请刷新页面后再填后 4 位。","err");
            return;
        }
        if(suffix.length!==4){
            setMsg("请填写 ICCID 后 4 位（0-9、A-F）。","err");
            return;
        }
        var v=prefix+suffix;
        if(v.length<18||v.length>20){
            setMsg("拼接后的 ICCID 长度不正确，请刷新页面重试。","err");
            return;
        }
        setBusy(true);
        setMsg("正在读取设备当前 ICCID 并比对…","");
        $.ajax({url:api(),type:"POST",data:{iccid:v},dataType:"json",timeout:15000})
        .done(function(r){
            setBusy(false);
            auth=r||auth;
            renderAuth();
            if(r&&r.ok){
                setMsg("后 4 位与设备一致，已授权。局域网无终端时自动开始烧流。","ok");
            }else{
                setMsg((r&&r.msg)||"授权失败。","err");
            }
            loadAuth(false);
        })
        .fail(function(xhr){
            setBusy(false);
            var r=xhr&&xhr.responseJSON;
            if(r&&r.current){
                auth={ok:false,current:r.current,manual:"",match:0,probe_ok:1};
                renderAuth();
                setMsg((r.msg||"ICCID 不一致，已拒绝")+"（当前 "+r.current+"）","err");
            }else{
                setMsg("授权请求失败，请检查设备连接。","err");
            }
            loadAuth(false);
        });
    }
    function stopAuth(){
        setBusy(true);
        $.ajax({url:api(),type:"POST",data:{action:"stop"},dataType:"json",timeout:10000})
        .done(function(r){
            setBusy(false);
            auth={ok:true,current:auth?auth.current:"",manual:"",match:0,probe_ok:1};
            $("#burnIccidInput").val("");
            renderAuth();
            setMsg((r&&r.msg)||"已停止并清除授权。","ok");
            load();
        })
        .fail(function(){
            setBusy(false);
            setMsg("停止请求失败，请检查设备连接。","err");
        });
    }
    function load(){
        if(!alive()){ if(iv){clearInterval(iv);iv=null;} if(av){clearInterval(av);av=null;} return; }
        $.ajax({url:"/js/burn_info.txt?r="+(new Date().getTime()),dataType:"text",cache:false,timeout:6000})
        .done(function(t){
            var p=String(t||"").trim().split("|");
            var total=parseInt(p[0],10)||0, st=p[1]==="1", last=parseInt(p[2],10)||0, imsi=(p[3]||"").trim();
            var stateText=st?"烧流中":(auth&&auth.match?"已授权 · 等待空闲":"空闲");
            $("#burnState").text(stateText).css("color", st?"#22c55e":(auth&&auth.match?"#38bdf8":"#e8f0f6"));
            $("#burnTotal").text(fmt(total));
            $("#burnLast").text(last?fmt(last):"\u2014");
            $("#burnImsi").text(imsi|| (auth&&auth.manual) ||"未授权");
            $("#burnTip").text("数据每 2 秒自动刷新 \u00b7 累计 "+(total/1073741824).toFixed(3)+" GB");
        })
        .fail(function(){
            $("#burnState").text("未检测到").css("color","#ef4444");
            $("#burnTip").text("读不到统计文件：burnctl 可能未安装或未启动");
        });
    }
    function tick(){ if(!alive()){ if(iv){clearInterval(iv);iv=null;} if(av){clearInterval(av);av=null;} return; } load(); }
    return {init:function(){
        load();
        loadAuth(false);
        if(iv){clearInterval(iv);} iv=setInterval(tick,2000);
        if(av){clearInterval(av);} av=setInterval(function(){loadAuth(false);},20000);
        $("#burnAuthBtn").off("click").on("click",submitAuth);
        $("#burnStopBtn").off("click").on("click",stopAuth);
    }};
});
