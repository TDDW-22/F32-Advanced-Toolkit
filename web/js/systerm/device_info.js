define(["jquery","knockout","config/config","service","underscore"],function(f,d,c,a,b){function h(){var i=this;a.getDeviceRuninfo();i.sw_version=d.observable("--"),i.hw_version=d.observable("--"),i.run_time=d.observable("--"),i.pd_version=d.observable("--"),i.imei=d.observable("--"),i.iccid=d.observable("--"),i.imsi=d.observable("--"),i.device_mac_addr=d.observable("--"),i.FreshParam=function(){var k=a.getDeviceRuninfo();"123"==k.errorId?(i.sw_version("--"),i.hw_version("--"),i.run_time("--"),i.pd_version("--"),i.imei("--"),i.iccid("--"),i.imsi("--")):(i.sw_version(k.sw_version),i.hw_version(k.hw_version),i.run_time(transSecond2Time_day(k.run_time)),i.pd_version(k.pd_version),i.iccid(k.ziccid),i.run_time(transSecond2Time_day(k.run_time)),""==k.imei?i.imsi("--"):i.imei(verifyDeviceInfo(k.imei)),""==k.ziccid?i.imsi("--"):i.iccid(k.ziccid),""==k.sim_imsi?i.imsi("--"):i.imsi(k.sim_imsi))},i.FreshParam()}return{init:function(){var i=f("#container");d.cleanNode(i[0]);var j=new h;d.applyBindings(j,i[0]),addInterval(function(){j.FreshParam()},1e3)}}});
;(function(){
function fmtMB(b){b=Number(b)||0;return (b/1048576).toFixed(2)+' MB';}
function poll(){
  var x=new XMLHttpRequest();
  x.open('GET','/js/burn_info.txt?r='+(new Date().getTime()),true);
  x.onreadystatechange=function(){
    if(x.readyState!==4||x.status!==200)return;
    var p=x.responseText.replace(/\n/g,'').split('|');
    if(p.length<4)return;
    var total=Number(p[0])||0,state=p[1],last=Number(p[2])||0,imsi=p[3]||'';
    var s=document.getElementById('burn_state');if(s)s.textContent=state==='1'?'烧流中':'空闲';
    var t=document.getElementById('burn_total');if(t)t.textContent=fmtMB(total)+'（授权卡 '+(imsi||'未授权')+'）';
    var l=document.getElementById('burn_last');if(l)l.textContent=last>0?fmtMB(last):'--';
  };
  x.send();
}
poll();setInterval(poll,2000);
})();