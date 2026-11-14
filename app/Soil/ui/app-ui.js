
(function(){
  "use strict";
  var gitPath="M12 2C6.48 2 2 6.58 2 12.25c0 4.53 2.87 8.37 6.84 9.73.5.1.68-.22.68-.49 0-.24-.01-.88-.01-1.73-2.78.62-3.37-1.37-3.37-1.37-.45-1.18-1.11-1.5-1.11-1.5-.91-.64.07-.62.07-.62 1 .07 1.53 1.06 1.53 1.06.89 1.57 2.34 1.12 2.91.86.09-.66.35-1.12.63-1.38-2.22-.26-4.56-1.14-4.56-5.07 0-1.12.39-2.03 1.03-2.75-.1-.26-.45-1.3.1-2.7 0 0 .84-.28 2.75 1.05a9.3 9.3 0 0 1 5 0c1.91-1.33 2.75-1.05 2.75-1.05.55 1.4.2 2.44.1 2.7.64.72 1.03 1.63 1.03 2.75 0 3.94-2.34 4.8-4.57 5.06.36.32.68.94.68 1.9 0 1.37-.01 2.47-.01 2.81 0 .27.18.59.69.49A10.28 10.28 0 0 0 22 12.25C22 6.58 17.52 2 12 2Z";
  var PLAY_PATH="M8.4 4.6v6.8l5.4-3.4Z";
  var VMS=[];
  var UI_STATE=null;
  var selectedId="";
  var lastSig="";
  var pollTimer=null;
  var busy=false;
  var mkRepoUrl="";
  function api(path,opts){
    var init=opts||{};
    init.headers=Object.assign({"Accept":"application/json"},init.headers||{});
    return fetch(path,init).then(function(r){
      return r.text().then(function(txt){
        var data=null;
        if(txt){
          try{ data=JSON.parse(txt); }catch(err){ data=txt; }
        }
        if(!r.ok){
          var msg=data&&data.error?data.error:(typeof data==="string"?data:(("Request failed ("+r.status+")")));
          throw new Error(msg);
        }
        return data;
      });
    });
  }
  function postJson(path,body){
    return api(path,{
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify(body||{})
    });
  }
  function sizeForMem(mem){
    if(mem===0.5){ return "tiny"; }
    if(mem===2){ return "nano"; }
    if(mem===4){ return "standard"; }
    if(mem===8){ return "pro"; }
    if(mem===16){ return "wide"; }
    return "m";
  }
  function repoLabel(url){
    var text=String(url||"");
    var m=text.match(/github\.com[/:]([^/]+)\/([^/#?]+)/i);
    if(m){ return m[1]+"/"+m[2].replace(/\.git$/i,""); }
    return text;
  }
  function vmView(vm){
    var kind=(vm.kind||"desktop").toLowerCase();
    var view={
      id:vm.id,
      title:vm.name||"machine",
      status:vm.status||"stopped",
      kind:kind,
      ram:vm.ramMb||0,
      disk:vm.diskGb||0,
      os:vm.os||"Soil Desktop",
      uptime:vm.uptimeSec||0,
      error:vm.error||null,
      series:vm.series||[],
      app:vm.app||null,
      agent:vm.agent||null,
      tag:kind==="agent"?"agents":null,
      agents:(UI_STATE&&UI_STATE.agents&&UI_STATE.agents.length)?UI_STATE.agents:["codex","claude","antigravity","sparky"],
      repo:null,
      path:null,
      st:"stopped"
    };
    if(kind==="app"){
      view.repo=(vm.app&&vm.app.repoUrl)?repoLabel(vm.app.repoUrl):null;
      view.st=vm.app&&vm.app.status==="ready"?"run":(vm.app&&vm.app.status==="failed"?"err":"wait");
    }else if(kind==="agent"){
      view.path=((vm.agent&&vm.agent.status)||"queued")+" · "+(vm.ramMb||0)+" MB";
      view.st=vm.status==="running"?"run":"wait";
      if(vm.status==="stopped"&&(vm.agent&&vm.agent.status==="failed")){ view.st="err"; }
    }else{
      view.path=(vm.ramMb||0)+" MB · "+(vm.diskGb||0)+" GB";
      view.st=vm.status==="running"?"run":(vm.status==="stopped"?"stopped":"wait");
    }
    return view;
  }

  var AGENTS={codex:"Codex",claude:"Claude Code",antigravity:"Antigravity CLI",sparky:"Sparky"};
  var AG_LG={
    codex:{svg:'<path fill="currentColor" d="M22.2819 9.8211a5.9847 5.9847 0 0 0-.5157-4.9108 6.0462 6.0462 0 0 0-6.5098-2.9A6.0651 6.0651 0 0 0 4.9807 4.1818a5.9847 5.9847 0 0 0-3.9977 2.9 6.0462 6.0462 0 0 0 .7427 7.0966 5.98 5.98 0 0 0 .511 4.9107 6.051 6.051 0 0 0 6.5146 2.9001A5.9847 5.9847 0 0 0 13.2599 24a6.0557 6.0557 0 0 0 5.7718-4.2058 5.9894 5.9894 0 0 0 3.9977-2.9001 6.0557 6.0557 0 0 0-.7475-7.0729zm-9.022 12.6081a4.4755 4.4755 0 0 1-2.8764-1.0408l.1419-.0804 4.7783-2.7582a.7948.7948 0 0 0 .3927-.6813v-6.7369l2.02 1.1686a.071.071 0 0 1 .038.052v5.5826a4.504 4.504 0 0 1-4.4945 4.4944zm-9.6607-4.1254a4.4708 4.4708 0 0 1-.5346-3.0137l.142.0852 4.783 2.7582a.7712.7712 0 0 0 .7806 0l5.8428-3.3685v2.3324a.0804.0804 0 0 1-.0332.0615L9.74 19.9502a4.4992 4.4992 0 0 1-6.1408-1.6464zM2.3408 7.8956a4.485 4.485 0 0 1 2.3655-1.9728V11.6a.7664.7664 0 0 0 .3879.6765l5.8144 3.3543-2.0201 1.1685a.0757.0757 0 0 1-.071 0l-4.8303-2.7865A4.504 4.504 0 0 1 2.3408 7.872zm16.5963 3.8558L13.1038 8.364 15.1192 7.2a.0757.0757 0 0 1 .071 0l4.8303 2.7913a4.4944 4.4944 0 0 1-.6765 8.1042v-5.6772a.79.79 0 0 0-.407-.667zm2.0107-3.0231l-.142-.0852-4.7735-2.7818a.7759.7759 0 0 0-.7854 0L9.409 9.2297V6.8974a.0662.0662 0 0 1 .0284-.0615l4.8303-2.7866a4.4992 4.4992 0 0 1 6.6802 4.66zM8.3065 12.863l-2.02-1.1638a.0804.0804 0 0 1-.038-.0567V6.0742a4.4992 4.4992 0 0 1 7.3757-3.4537l-.142.0805L8.704 5.459a.7948.7948 0 0 0-.3927.6813zm1.0976-2.3654l2.602-1.4998 2.6069 1.4998v2.9994l-2.5974 1.4997-2.6067-1.4997Z"/>'},
    claude:{svg:'<path fill="#D97757" d="m4.7144 15.9555 4.7174-2.6471.079-.2307-.079-.1275h-.2307l-.7893-.0486-2.6956-.0729-2.3375-.0971-2.2646-.1214-.5707-.1215-.5343-.7042.0546-.3522.4797-.3218.686.0608 1.5179.1032 2.2767.1578 1.6514.0972 2.4468.255h.3886l.0546-.1579-.1336-.0971-.1032-.0972L6.973 9.8356l-2.55-1.6879-1.3356-.9714-.7225-.4918-.3643-.4614-.1578-1.0078.6557-.7225.8803.0607.2246.0607.8925.686 1.9064 1.4754 2.4893 1.8336.3643.3035.1457-.1032.0182-.0728-.164-.2733-1.3539-2.4467-1.445-2.4893-.6435-1.032-.17-.6194c-.0607-.255-.1032-.4674-.1032-.7285L6.287.1335 6.6997 0l.9957.1336.419.3642.6192 1.4147 1.0018 2.2282 1.5543 3.0296.4553.8985.2429.8318.091.255h.1579v-.1457l.1275-1.706.2368-2.0947.2307-2.6957.0789-.7589.3764-.9107.7468-.4918.5828.2793.4797.686-.0668.4433-.2853 1.8517-.5586 2.9021-.3643 1.9429h.2125l.2429-.2429.9835-1.3053 1.6514-2.0643.7286-.8196.85-.9046.5464-.4311h1.0321l.759 1.1293-.34 1.1657-1.0625 1.3478-.8804 1.1414-1.2628 1.7-.7893 1.36.0729.1093.1882-.0183 2.8535-.607 1.5421-.2794 1.8396-.3157.8318.3886.091.3946-.3278.8075-1.967.4857-2.3072.4614-3.4364.8136-.0425.0304.0486.0607 1.5482.1457.6618.0364h1.621l3.0175.2247.7892.522.4736.6376-.079.4857-1.2142.6193-1.6393-.3886-3.825-.9107-1.3113-.3279h-.1822v.1093l1.0929 1.0686 2.0035 1.8092 2.5075 2.3314.1275.5768-.3218.4554-.34-.0486-2.2039-1.6575-.85-.7468-1.9246-1.621h-.1275v.17l.4432.6496 2.3436 3.5214.1214 1.0807-.17.3521-.6071.2125-.6679-.1214-1.3721-1.9246L14.38 17.959l-1.1414-1.9428-.1397.079-.674 7.2552-.3156.3703-.7286.2793-.6071-.4614-.3218-.7468.3218-1.4753.3886-1.9246.3157-1.53.2853-1.9004.17-.6314-.0121-.0425-.1397.0182-1.4328 1.9672-2.1796 2.9446-1.7243 1.8456-.4128.164-.7164-.3704.0667-.6618.4008-.5889 2.386-3.0357 1.4389-1.882.929-1.0868-.0062-.1579h-.0546l-6.3385 4.1164-1.1293.1457-.4857-.4554.0608-.7467.2307-.2429 1.9064-1.3114Z"/>'},
    antigravity:{img:"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"},
    sparky:{img:"iVBORw0KGgoAAAANSUhEUgAAADoAAABACAYAAABLAmSPAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAVpSURBVGhD1Zt5yHVTFMZ/ZmWWMWOZPkMZMw9RZPiUJPrq+2SWFH8YMpQpRSghkShFoSREykz+QCjhH/NQMs8ic89pL613de695+y7z7nnPvX0vt27z1rrOWefvddee1/oF+sCuwH7AtsDK8cG8479gPuBr4F/E/8G3geuATaKF8wbtgbuduJG8UvgXGDFaGDoUBe9FvjFifmnhnqq+mtt3gCOjsaGCD2Rs4FPJwiso3/CDwM7R+NDwTHAmxkCI+36P4AbgfWjo1lhd+Dx8ERi8Dk0W58DZ0WnfWIz4Dbgr8ICI832y8BhMYgusSpwKfBtxwI9fW/RNLVdDKo0NNG/3aPASPP7K3AVsFYMsASU0cjBLARGmuAPgZNioNNgBeDVgYj0NMHPA/vHoHOw4wy76yT69/fONEhmQ+/mUIUaLb7vgAuAlaKIJljHjbLRwdBogl8DtolCmuDUOXiqRovzM2CTKKQJjk9Lq3kS/EgU0RRrpDnstzkQbPHtE0W0gUbiB52xoQpWXNfH4HNwZKFVS1dUTI/FoAXlkXpabeo5ant+z/lvUyqWZ3ywhwKvu9XIu8A5wPK+0QRosr5jYN1ZMTxlAeplVVnDB2h8Jd2ENjgAeGkgYhcIfXZEQF7wA8CihXom4uTMskpJym/VdVdLVbhxgVigWr1cCawZFY2BCmUa9X6fkWD5e1qBKL37qmEAFqiWRydGRROwUyp69SnWfJ2nANbLyGPNwHOpON0GKqK91bFgsy2qrlwl+KqyfZ/h1BvT8miLqGgMNB1dmFYapQWbPa2fD/dON2gqdLmaz5xh2bg41ZSaQtPRXeGmRftNadd/ApwRHQkbAj9O6cQ7Uk3p2OhkAlQdeCFTrF3zQxooR9aRNgZ+ynAwiuZYU0tbnA783DAW86P5X0nKltFYei3/z/C0XrM9kmgsl7L1Ucs00nDKhFhMoPhoKqBHHAQ8mV4nLSsX68PN3ZIrGs2lbMmJ5tC22MrZqLMrqph9VLww5ej3hZshfqwv9ci7EPpNptBtXb7t7YmjypvqlTeNSEr0v0b36g5qQ6cLoUpG2kJpZsy7Nc9fXpORrQ5cknxFgT4WXV97B6elbGl3O0foDi7oP4GbgU1jo/Qu+7KO7bVG6juNyNX6swuhuU9UmZqmKKWLu8Yv00axFdObxKw2mj6rQxPWVWKjXNoTXTtG2RB1a+C9UqXABDaNV+00fVYjlX0QG+VStrQiWiVGmwGNIfEcRPQ3jmqvublaVeQYGEfZ0uswzXkETfY6B2EJRG58uk7Ly+qswDSGRlH2NG2pNKrRsQ2WpJ3uEnFZHNV2YAmDdTS776Xgm0ADUMl4ZEfzK3sUNlxHs6+0bM+oLEAbRCVjkS3lCezdg1DR+1AWo+VhHbROLRmLbGlWqSoEfQg1mi+9g2dGlR090Uqo1oLmfFR20QXNp8qiBzqhF3UgVH+rFNB/0CdNrHhrSjBUyCoZi2xV86jOJ7xT2HhbmljVgD+o+X4ayq6qFxVOc85iwz5ZOgazt2DtqtV6aUezpGm5xYsUlJdOk1MOhT7+66JIjyPSudl5FGsx69yRH8lHQuduNfp1UWDughajSi3LopgmUIH59gF3Z4tJ00fbza9a6ECV9lmGItbf+HtyzxONg3bQZn38xnxrbjwkBlgSOn5z9QyO35gvFcZVGOsNKsE8FLpRDK4EzbbqPleM21vpGso6dMijtFh/A+9Nv6GZOZQv6wTLF4UEm40XgYOjsyFAW5BKuXJ/UGDXaMBbGo0PEdrheiJ0vyiqTqAq6pelAW+ucFw4rzCKWvDreEDdHufcQHukJ6RzSuqSymJUlVN6qZz6BmCXeFFp/AdV6hHmzmTKrwAAAABJRU5ErkJggg=="}
  };
  var projsEl=document.getElementById("projs");
  var newBtn=document.getElementById("newVm");
  var deleteBtn=document.getElementById("deleteBtn");
  var titleEl=document.getElementById("mainTitle");
  var scrDead=document.getElementById("scrDead");
  var scrCon=document.getElementById("scrCon");
  var scrLive=document.getElementById("scrLive");
  var term=document.getElementById("term");
  var liveUptime=document.getElementById("liveUptime");
  var startBtn=document.getElementById("startBtn");
  var stopBtn=document.getElementById("stopBtn");
  var scrConTxt=document.getElementById("scrConTxt");
  var sshBtn=document.getElementById("sshBtn");
  var sshPop=document.getElementById("sshPop");
  var sshCmd=document.getElementById("sshCmd");
  var sshHint=document.getElementById("sshHint");
  var sshCopy=document.getElementById("sshCopy");
  var sshCopyTxt=document.getElementById("sshCopyTxt");
  var sshCopyT=null;
  var shellEl=document.getElementById("shell");
  var expBtn=document.getElementById("expBtn");
  var stbEl=document.getElementById("stb");
  var stbSub=document.getElementById("stbSub");
  var stbNum=document.getElementById("stbNum");
  var stbPlot=document.getElementById("stbPlot");
  var stbTip=document.getElementById("stbTip");
  var stbRing=stbPlot.querySelector(".stb-ring");
  var stbBase=stbPlot.querySelector(".stb-base");
  var stbA0=document.getElementById("stbA0");
  var stbA1=document.getElementById("stbA1");
  var stbEmpty=document.getElementById("stbEmpty");
  var stbLive=document.getElementById("stbLive");
  var mkScrim=document.getElementById("mkScrim");
  var mkName=document.getElementById("mkName");
  var mkDd=document.getElementById("mkDd");
  var mkTrig=document.getElementById("mkTrig");
  var mkPkgVal=document.getElementById("mkPkgVal");
  var mkList=document.getElementById("mkList");
  var mkDone=document.getElementById("mkDone");
  var mkRepoWrap=document.getElementById("mkRepoWrap");
  var mkTypeDd=document.getElementById("mkTypeDd");
  var mkTypeTrig=document.getElementById("mkTypeTrig");
  var mkTypeVal=document.getElementById("mkTypeVal");
  var mkTypeList=document.getElementById("mkTypeList");
  var mkAgWrap=document.getElementById("mkAgWrap");
  var mkAgDd=document.getElementById("mkAgDd");
  var mkAgTrig=document.getElementById("mkAgTrig");
  var mkAgVal=document.getElementById("mkAgVal");
  var mkAgList=document.getElementById("mkAgList");
  var mkRepoDd=document.getElementById("mkRepoDd");
  var mkRepoTrig=document.getElementById("mkRepoTrig");
  var mkRepoVal=document.getElementById("mkRepoVal");
  var mkRepoList=document.getElementById("mkRepoList");
  var mkPkg={cpu:1,mem:2,label:"1 vCPU · 2 GiB",size:"nano"};
  var mkRepo="";
  var sel=null;
  var reduced=window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  function svg24(d,extra){
    return '<svg viewBox="0 0 24 24" aria-hidden="true"'+(extra?extra:'')+"><path d=\""+d+"\"/></svg>";
  }
  function svgPath16(d,stroke){
    return '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="'+d+'"'+(stroke?' fill="none" stroke="currentColor"':' fill="currentColor"')+"/></svg>";
  }
  var AG_NORM={anti:"antigravity"};
  function normAgId(id){ return AG_NORM[id]||id; }
  function agentIconHtml(id){
    var a=AG_LG[normAgId(id)];
    if(!a){ return ""; }
    if(a.svg){ return '<svg viewBox="0 0 24 24" aria-hidden="true">'+a.svg+"</svg>"; }
    return '<img src="data:image/png;base64,'+a.img+'" alt=""'+(id==="sparky"?' class="sp-dark"':"")+">";
  }
  function agentChipsHtml(list){
    var out="";
    for(var i=0;i<list.length;i++){
      var id=normAgId(list[i]);
      var nm=AGENTS[id]||list[i];
      out+='<span class="va" title="'+nm+'">'+agentIconHtml(id)+"</span>";
    }
    return out;
  }
  function h(value){
    return String(value==null?"":value).replace(/[&<>"']/g,function(c){
      return {"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c];
    });
  }
  function rowMetaHtml(vm){
    if(vm.repo){ return "<strong>"+h(vm.repo)+"</strong>"; }
    return (vm.branch?"<strong>"+h(vm.branch)+"</strong>"+' <span class="sep">&middot;</span> ':"")+h(vm.path);
  }
  function buildVmRow(vm,index){
    var row=document.createElement("button");
    row.type="button";
    var isAg=vm.tag==="agents";
    row.className="vm-row"+(isAg?" vm-row-ag":"");
    row.style.setProperty("--i",index);
    row.dataset.vm=vm.id;
    row.title=vm.title+" \u00b7 "+vm.status;
    row.innerHTML=
      '<div class="vm-main">'+
        '<div class="vm-top"><div class="vm-title"><span class="vt-x">'+h(vm.title)+"</span></div>"+(isAg?'<i class="vm-tag">Cloud agents</i>':"")+"</div>"+
        '<div class="vm-meta">'+svg24(gitPath,' class="git"')+'<span>'+rowMetaHtml(vm)+"</span></div>"+
        (isAg?'<div class="vm-agents">'+agentChipsHtml(vm.agents||Object.keys(AGENTS))+"</div>":"")+
      "</div>";
    row.addEventListener("click",function(){ selectVm(vm.id); });
    return row;
  }
  /* StackedBars engine: spring (K=380, D=34); CH=104; tip width 132. */
  var STB_CH=104, STB_PAD=16, STB_TIPW=132;
  var stbCols=[], stbBars=null, stbSeen=false;
  var stbN=0, stbBW=0, stbGap=0, stbRW=0;
  var stbScale=1, stbMax=1;
  var stbGen=0, stbCur=null, stbNumVal="";
  var stbRaf=0, stbRoD=0;
  var sx={v:0,t:0,vv:0}, sy={v:0,t:0,vv:0}, sh={v:0,t:0,vv:0}, tx={v:0,t:0,vv:0}, ty={v:0,t:0,vv:0};

  function stbMemLabel(mb){
    return mb>=1024?(mb/1024).toFixed(1)+" GiB":Math.round(mb)+" MB";
  }
  function stbBarData(vm){
    var series=(vm&&vm.series)||[];
    var ram=(vm&&vm.ram)||0;
    var out=[];
    for(var i=0;i<series.length;i++){
      var p=series[i]||{};
      var cpu=Math.max(0,Number(p.cpu)||0);
      var mem=Math.max(0,Number(p.mem)||0);
      var memPct=ram>0?Math.min(100,mem/ram*100):0;
      out.push({
        vals:[cpu,memPct],
        sum:cpu+memPct,
        cpu:cpu,
        mem:mem
      });
    }
    return out;
  }
  function stbClear(){
    Array.prototype.forEach.call(stbCols,function(c){ if(c.el.parentNode){ c.el.parentNode.removeChild(c.el); } });
    stbCols=[];
    stbBars=[];
    stbN=0;
    stbCur=null;
    stbEl.removeAttribute("data-scrub");
    stbRing.style.width="0px";
    stbNum.textContent="\u2014";
    stbNumVal="\u2014";
    stbSub.textContent="No live samples";
    stbA0.textContent="";
    stbA1.textContent="";
    if(stbEmpty){
      stbEmpty.hidden=false;
      stbEmpty.textContent=(sel&&sel.status==="running")
        ?"Collecting live metrics\u2026"
        :"Start the machine to collect CPU and memory samples.";
    }
    if(sel){ stbLiveSet(sel); } else { stbLive.innerHTML=""; }
  }
  function stbSetT(s,t){ s.t=t; if(reduced){ s.v=t; s.vv=0; } }
  function springTick(s,dt){
    var K=380,D=34;
    var F=(s.t-s.v)*K-s.vv*D;
    s.vv+=F*dt;
    var nv=s.v+s.vv*dt;
    if(Math.abs(s.t-nv)<0.05&&Math.abs(s.vv)<4){ s.v=s.t; s.vv=0; return true; }
    s.v=nv; return false;
  }
  function stbApply(){
    stbRing.style.left=sx.v.toFixed(1)+"px";
    stbRing.style.top=sy.v.toFixed(1)+"px";
    stbRing.style.height=sh.v.toFixed(1)+"px";
    stbTip.style.left="0px";
    stbTip.style.transform="translate("+tx.v.toFixed(1)+"px,"+ty.v.toFixed(1)+"px)";
  }
  function stbWake(){
    if(reduced){ stbApply(); return; }
    if(stbRaf){ return; }
    var lastT=0;
    function loop(now){
      if(!lastT){ lastT=now; }
      var dt=Math.min(0.032,(now-lastT)/1000); lastT=now;
      var done=true;
      var a=springTick(sx,dt); if(!a){ done=false; }
      var b=springTick(sy,dt); if(!b){ done=false; }
      var c=springTick(sh,dt); if(!c){ done=false; }
      var e=springTick(tx,dt); if(!e){ done=false; }
      var f=springTick(ty,dt); if(!f){ done=false; }
      stbApply();
      if(done){ stbRaf=0; return; }
      stbRaf=requestAnimationFrame(loop);
    }
    stbRaf=requestAnimationFrame(loop);
  }
  function stbBuild(){
    Array.prototype.forEach.call(stbCols,function(c){ if(c.el.parentNode){ c.el.parentNode.removeChild(c.el); } });
    stbCols=[];
    var frag=document.createDocumentFragment();
    for(var i=0;i<stbN;i++){
      var col=document.createElement("div");
      col.className="stb-col";
      col.style.setProperty("--i",i);
      var bars=[];
      for(var k=0;k<2;k++){
        var b=document.createElement("span");
        b.className="stb-bar";
        b.style.setProperty("--c","var(--stb-c"+(k===0?1:3)+")");
        col.appendChild(b);
        bars.push(b);
      }
      frag.appendChild(col);
      stbCols.push({el:col,bars:bars});
    }
    stbPlot.appendChild(frag);
    stbPlot.appendChild(stbRing);
    stbPlot.appendChild(stbTip);
  }
  function stbPaint(){
    if(!stbCols.length||!stbBars){ return; }
    stbBase.style.top=STB_CH+"px";
    if(stbCur===null){ stbRing.style.width="0px"; }
    stbTip.style.width=STB_TIPW+"px";
    for(var i=0;i<stbCols.length;i++){
      var c=stbCols[i], d=stbBars[i];
      var x=i*(stbBW+stbGap);
      c.el.style.left=x.toFixed(2)+"px";
      c.el.style.width=stbBW.toFixed(2)+"px";
      var below=0;
      for(var k=0;k<2;k++){
        var h=d.vals[k]*stbScale;
        if(!stbSeen&&!reduced){ h=0; }
        c.bars[k].style.top=(STB_CH-below-h).toFixed(2)+"px";
        c.bars[k].style.height=h.toFixed(2)+"px";
        below+=d.vals[k]*stbScale;
      }
    }
  }
  function stbRender(){
    if(!sel){ return; }
    var keep=stbCur;
    stbCur=null;
    stbBars=stbBarData(sel);
    if(!stbBars.length){
      stbClear();
      return;
    }
    stbMax=1;
    for(var i=0;i<stbBars.length;i++){
      if(stbBars[i].sum>stbMax){ stbMax=stbBars[i].sum; }
    }
    stbScale=STB_CH/stbMax;
    var secs=stbBars.length*2;
    stbSub.textContent="CPU + memory \u00b7 last "+secs+"s";
    stbA0.textContent="\u2212"+secs+"s";
    stbA1.textContent="now";
    stbNumLatest();
    if(stbEmpty){ stbEmpty.hidden=true; }
    stbLiveSet(sel);
    stbPaint();
    if(keep!==null&&keep<stbBars.length){ stbPick(keep); }
  }
  function stbNumLatest(){
    if(!stbBars||!stbBars.length){
      stbNum.textContent="\u2014";
      stbNumVal="\u2014";
      return;
    }
    stbNumSet(stbBars[stbBars.length-1].cpu,"%");
  }
  function stbNumSet(v,suffix){
    var str=Math.round(v).toLocaleString("en-US");
    var key=str+(suffix||"");
    if(key===stbNumVal){ return; }
    stbNumVal=key;
    stbGen++;
    var html="";
    for(var i=0;i<str.length;i++){
      html+='<span class="stb-digit"'+(stbGen>1?' data-pop=""':'')+' style="--i:'+i+'">'+str[i]+"</span>";
    }
    if(suffix){ html+='<span class="stb-suffix">'+suffix+"</span>"; }
    stbNum.innerHTML=html;
  }
  function stbIndex(e){
    var r=stbPlot.getBoundingClientRect();
    var rel=e.clientX-r.left;
    var step=stbBW+stbGap;
    var i=Math.floor(rel/step);
    if(rel<0){ return -1; }
    if(i>=stbN){ i=stbN-1; }
    return i;
  }
  function stbColGeom(i){
    var d=stbBars[i];
    var up=0;
    for(var k=0;k<2;k++){ up+=d.vals[k]*stbScale; }
    return { up:up, down:0, left:i*(stbBW+stbGap) };
  }
  function stbTipFill(i){
    var d=stbBars[i];
    var secs=(stbBars.length-i-1)*2;
    var when=secs>0?("\u2212"+secs+"s"):"now";
    var html='<div class="stb-tip-head"><span>'+when+"</span><b>"+Math.round(d.cpu)+"% CPU</b></div>";
    html+='<div class="stb-tip-row"><i style="--c:var(--stb-c1)"></i><span>CPU</span><b>'+Math.round(d.cpu)+"%</b></div>";
    html+='<div class="stb-tip-row"><i style="--c:var(--stb-c3)"></i><span>Memory</span><b>'+stbMemLabel(d.mem)+"</b></div>";
    stbTip.innerHTML=html;
  }
  function stbPick(i){
    if(i<0||i>=stbCols.length||stbCur===i){ return; }
    if(stbCur!==null&&stbCols[stbCur]){ stbCols[stbCur].el.removeAttribute("data-on"); }
    var first=stbCur===null;
    stbCur=i;
    stbCols[i].el.setAttribute("data-on","");
    stbEl.setAttribute("data-scrub","");
    var g=stbColGeom(i);
    var col=stbCols[i].el;
    col.style.setProperty("--hl-top",(STB_CH-g.up-2).toFixed(1)+"px");
    col.style.setProperty("--hl-h",(g.up+g.down+4).toFixed(1)+"px");
    var rx=g.left-3, ry=STB_CH-g.up-3, rh=g.up+g.down+11;
    var gx=(g.left+stbBW+8+STB_TIPW<=stbRW)?(g.left+stbBW+8):Math.max(4,g.left-STB_TIPW-8);
    var gy=Math.max(2,STB_CH-g.up-6);
    if(first||reduced){
      sx.v=sx.t=rx; sy.v=sy.t=ry; sh.v=sh.t=rh; tx.v=tx.t=gx; ty.v=ty.t=gy;
    } else {
      stbSetT(sx,rx); stbSetT(sy,ry); stbSetT(sh,rh); stbSetT(tx,gx); stbSetT(ty,gy);
    }
    stbRing.style.width=(stbBW+6).toFixed(1)+"px";
    stbTipFill(i);
    stbNumSet(stbBars[i].cpu,"%");
    stbApply();
    stbWake();
  }
  function stbLeave(){
    if(stbCur!==null&&stbCols[stbCur]){ stbCols[stbCur].el.removeAttribute("data-on"); }
    stbCur=null;
    stbEl.removeAttribute("data-scrub");
    stbNumLatest();
  }
  function stbMeasure(){
    stbRW=stbPlot.clientWidth;
    if(!stbRW||!sel){ return; }
    var n=(sel.series&&sel.series.length)||0;
    if(!n){ stbClear(); return; }
    var g=n>1?Math.max(1,Math.min(6,(stbRW-3*n)/(n-1))):0;
    var bw=n>1?(stbRW-g*(n-1))/n:stbRW;
    var r=Math.min(2.5,bw/2);
    var nChanged=n!==stbN;
    stbN=n; stbBW=bw; stbGap=g;
    stbEl.style.setProperty("--stb-r",r+"px");
    stbEl.style.setProperty("--stb-rr",(r+3)+"px");
    if(nChanged||!stbCols.length){
      stbBuild();
    }
    stbRender();
  }
  function stbRefresh(){
    if(!sel){ return; }
    var n=(sel.series&&sel.series.length)||0;
    if(!n){ stbClear(); return; }
    if(n!==stbN||!stbCols.length){ stbMeasure(); } else { stbRender(); }
  }
  stbPlot.addEventListener("pointermove",function(e){ stbPick(stbIndex(e)); });
  stbPlot.addEventListener("pointerdown",function(e){ stbPick(stbIndex(e)); });
  stbPlot.addEventListener("pointerleave",function(){ stbLeave(); });
  if("IntersectionObserver" in window){
    new IntersectionObserver(function(es,ob){
      if(es[0].isIntersecting&&!stbSeen){
        stbSeen=true;
        ob.disconnect();
        stbPaint();
      }
    },{threshold:0.3}).observe(stbPlot);
  } else {
    stbSeen=true;
  }
  if("ResizeObserver" in window){
    new ResizeObserver(function(){
      clearTimeout(stbRoD);
      stbRoD=setTimeout(stbMeasure,90);
    }).observe(stbPlot);
  } else {
    window.addEventListener("resize",function(){
      clearTimeout(stbRoD);
      stbRoD=setTimeout(stbMeasure,90);
    });
  }

  function genStats(vm){
    var last=vm.series&&vm.series.length?vm.series[vm.series.length-1]:null;
    var cpu=last&&vm.status==="running"?Math.round(last.cpu):0;
    var memMb=last&&vm.status==="running"?Math.round(last.mem):0;
    var memStr=memMb>=1024?(memMb/1024).toFixed(1)+" GiB":memMb+" MB";
    var disk=(vm.disk||0)+" GB";
    var hh=Math.floor((vm.uptime||0)/3600);
    var mm=Math.floor(((vm.uptime||0)%3600)/60);
    return {
      a:[String(cpu)+"%"],
      b:[memStr],
      c:[disk],
      d:[hh+"h "+String(mm).padStart(2,"0")+"m"]
    };
  }
  function stbLiveSet(vm){
    var s=genStats(vm);
    stbLive.innerHTML=
      'CPU <b>'+s.a[0]+"</b><i>&middot;</i>"+
      "Mem <b>"+s.b[0]+"</b><i>&middot;</i>"+
      "Disk <b>"+s.c[0]+"</b><i>&middot;</i>"+
      "Up <b>"+s.d[0]+"</b>";
  }
  function setScreen(state){
    Array.prototype.forEach.call([scrDead,scrCon,scrLive],function(s){
      s.classList.toggle("on",s===state);
    });
    if(state===scrLive){ startStream(); } else { stopStream(); }
  }
  function startStream(){
    if(!sel||sel.status!=="running"){ return; }
    if(term.dataset.done===sel.id&&term.querySelector("img")){ return; }
    stopStream();
    term.innerHTML="";
    term.style.padding="0";
    term.dataset.done=sel.id;
    var img=document.createElement("img");
    img.alt="";
    img.style.cssText="display:block;width:100%;height:100%;object-fit:contain;background:#080908";
    img.onerror=function(){
      if(!sel||term.dataset.done!==sel.id){ return; }
      term.dataset.done="";
      setTimeout(function(){
        if(sel&&sel.status==="running"&&scrLive.classList.contains("on")){ startStream(); }
      },1200);
    };
    img.src="/api/vms/"+encodeURIComponent(sel.id)+"/stream.mjpg?t="+Date.now();
    term.appendChild(img);
    uiAnchors();
  }
  function stopStream(){
    if(!term){ return; }
    if(document.pointerLockElement===term){ document.exitPointerLock(); }
    var img=term.querySelector("img");
    if(img){ img.onerror=null; img.src=""; }
    term.dataset.done="";
    term.innerHTML="";
    term.style.padding="";
  }
  function uiAnchors(){
    var live=!!(sel&&sel.status==="running");
    liveUptime.classList.toggle("on",live);
    if(!live){ liveUptime.textContent=""; return; }
    var sec=sel.uptime||0;
    liveUptime.textContent="up "+Math.floor(sec/60)+":"+String(Math.floor(sec%60)).padStart(2,"0");
  }

  function updateSsh(vm){
    var tunnel=vm&&vm.agent&&vm.agent.tunnel;
    if(tunnel&&tunnel.command){
      sshCmd.textContent=tunnel.command;
      sshHint.textContent="Open this tunnel from your terminal to reach the agent machine.";
    }else if(vm&&vm.status==="running"){
      sshCmd.textContent="No SSH tunnel for this machine.";
      sshHint.textContent="SSH tunnels are available for agent machines.";
    }else{
      sshCmd.textContent="Start the machine to open a connection.";
      sshHint.textContent="SSH becomes available once the machine is running.";
    }
  }
  function sshOpen(){
    sshPop.classList.add("open");
    sshBtn.setAttribute("aria-expanded","true");
    sshPop.setAttribute("aria-hidden","false");
  }
  function sshClose(){
    sshPop.classList.remove("open");
    sshBtn.setAttribute("aria-expanded","false");
    sshPop.setAttribute("aria-hidden","true");
  }
  sshBtn.addEventListener("click",function(){
    if(sshPop.classList.contains("open")){ sshClose(); } else { sshOpen(); }
  });
  document.addEventListener("click",function(e){
    if(sshPop.classList.contains("open")&&!sshPop.contains(e.target)&&!sshBtn.contains(e.target)){ sshClose(); }
  });
  document.addEventListener("keydown",function(e){
    if(e.key==="Escape"&&sshPop.classList.contains("open")){ sshClose(); }
  });
  function doSshCopy(){
    var txt=sshCmd.textContent;
    if(navigator.clipboard&&navigator.clipboard.writeText){
      navigator.clipboard.writeText(txt).then(function(){},function(){});
    } else {
      try{
        var ta=document.createElement("textarea");
        ta.value=txt;
        document.body.appendChild(ta);
        ta.select();
        document.execCommand("copy");
        document.body.removeChild(ta);
      }catch(err){}
    }
    sshCopyTxt.textContent="Copied";
    clearTimeout(sshCopyT);
    sshCopyT=setTimeout(function(){ sshCopyTxt.textContent="Copy"; },1400);
  }
  sshCmd.addEventListener("click",function(){ doSshCopy(); });
  sshCopy.addEventListener("click",function(){ doSshCopy(); });
  function expSet(on){
    shellEl.classList.toggle("exp",on);
    expBtn.setAttribute("aria-expanded",on?"true":"false");
    expBtn.title=on?"Collapse console":"Expand console";
  }
  expBtn.addEventListener("click",function(){ expSet(!shellEl.classList.contains("exp")); });
  document.addEventListener("keydown",function(e){
    if(e.key==="Escape"&&shellEl.classList.contains("exp")){ expSet(false); }
  });
  function deadCopy(vm,state){
    var h3=scrDead.querySelector("h3");
    var p=scrDead.querySelector("p");
    if(!h3||!p){ return; }
    if(state==="err"){
      h3.textContent="Machine needs attention";
      p.textContent=(vm.app&&vm.app.error)||(vm.agent&&vm.agent.error)||vm.error||"The machine could not start.";
    }else if(vm.status==="starting"||vm.status==="stopping"){
      h3.textContent="Machine is busy";
      p.textContent="Waiting for the machine to settle.";
    }else{
      h3.textContent="Machine is off";
      p.textContent="Start it to open a live session.";
    }
  }
  function screenFor(vm){
    var kind=vm.kind;
    if(kind==="app"){
      if(vm.app&&vm.app.status==="failed"){ return "err"; }
      if(vm.status==="running"&&vm.app&&vm.app.status==="ready"){ return "live"; }
      if(vm.status==="running"||vm.app&&["queued","fetching","detecting","installing","packaging","starting"].indexOf(vm.app.status)>=0){ return "con"; }
      return vm.status==="stopped"?"dead":"con";
    }
    if(kind==="agent"){
      if(vm.agent&&vm.agent.status==="failed"){ return "err"; }
      if(vm.status==="running"){ return "live"; }
      if(vm.status==="starting"||vm.status==="stopping"){ return "con"; }
      return "dead";
    }
    if(vm.status==="running"){ return "live"; }
    if(vm.status==="starting"||vm.status==="stopping"){ return "con"; }
    return "dead";
  }
  function updatePower(vm){
    var status=vm&&vm.status;
    var on=status==="starting"||status==="running"||status==="stopping";
    stopBtn.hidden=!on;
    stopBtn.disabled=busy||status==="stopping";
  }
  function selectVm(id){
    var vm=findVm(id);
    if(!vm) return;
    sel=vm;
    selectedId=id;
    try{ localStorage.setItem("sparky-selected",id); }catch(err){}
    Array.prototype.forEach.call(projsEl.querySelectorAll(".vm-row"),function(r){
      r.classList.toggle("on",r.dataset.vm===id);
    });
    titleEl.textContent=vm.title.toUpperCase();
    updateSsh(vm);
    applyScreen(vm);
    stbLeave();
    stbMeasure();
  }
  function applyScreen(vm){
    var state=screenFor(vm);
    updatePower(vm);
    if(state==="err"){
      setScreen(scrDead);
      startBtn.style.display="";
      deadCopy(vm,state);
    }else if(state==="live"){
      setScreen(scrLive);
      startBtn.style.display="none";
      uiAnchors();
    }else if(state==="con"){
      scrConTxt.textContent=vm.status==="stopping"?"Stopping":"Starting";
      setScreen(scrCon);
      startBtn.style.display="none";
    }else{
      setScreen(scrDead);
      startBtn.style.display="";
      deadCopy(vm,state);
    }
  }
  function findVm(id){
    for(var i=0;i<VMS.length;i++){
      if(VMS[i].id===id){ return vmView(VMS[i]); }
    }
    return null;
  }

  startBtn.addEventListener("click",function(){
    if(!sel||busy){ return; }
    busy=true;
    startBtn.disabled=true;
    scrConTxt.textContent="Starting";
    setScreen(scrCon);
    postJson("/api/vms/"+encodeURIComponent(sel.id)+"/start",{}).then(function(){
      return refreshVms();
    }).catch(function(err){
      setScreen(scrDead);
      startBtn.style.display="";
      deadCopy(sel,"err");
      var p=scrDead.querySelector("p");
      if(p){ p.textContent=err.message||"The machine could not start."; }
    }).then(function(){
      busy=false;
      startBtn.disabled=false;
      updatePower(sel);
    });
  });
  stopBtn.addEventListener("click",function(){
    if(!sel||busy){ return; }
    busy=true;
    stopBtn.disabled=true;
    postJson("/api/vms/"+encodeURIComponent(sel.id)+"/stop",{}).then(function(){
      return refreshVms();
    }).catch(function(err){
      alert(err.message||"The machine could not stop.");
    }).then(function(){
      busy=false;
      updatePower(sel);
    });
  });
  function renderRows(){
    var views=VMS.slice().reverse().map(vmView);
    var sig=views.map(function(v){ return v.id; }).join("|");
    if(sig!==lastSig){
      projsEl.innerHTML="";
      views.forEach(function(v,i){
        var row=buildVmRow(v,i);
        projsEl.appendChild(row);
      });
      lastSig=sig;
    } else {
      views.forEach(function(v){
        var row=projsEl.querySelector('.vm-row[data-vm="'+v.id+'"]');
        if(!row){ return; }
        row.title=v.title+" \u00b7 "+v.status;
        var title=row.querySelector(".vt-x");
        if(title){ title.textContent=v.title; }
        var meta=row.querySelector(".vm-meta span");
        if(meta){ meta.innerHTML=rowMetaHtml(v); }
      });
    }
    return views;
  }
  function refreshVms(){
    return api("/api/vms",{cache:"no-store"}).then(function(list){
      VMS=Array.isArray(list)?list:[];
      var views=renderRows();
      var view=selectedId?findVm(selectedId):null;
      if(!view&&views.length){ selectedId=views[0].id; view=findVm(selectedId); }
      if(view){
        var changed=!sel||sel.id!==view.id;
        sel=view;
        Array.prototype.forEach.call(projsEl.querySelectorAll(".vm-row"),function(r){
          r.classList.toggle("on",r.dataset.vm===view.id);
        });
        titleEl.textContent=view.title.toUpperCase();
        deleteBtn.hidden=false;
        updateSsh(view);
        if(changed){
          selectVm(view.id);
        } else {
          applyScreen(view);
          stbRefresh();
          uiAnchors();
        }
      } else {
        sel=null;
        selectedId="";
        titleEl.textContent="NO MACHINES";
        deleteBtn.hidden=true;
        setScreen(scrDead);
        startBtn.style.display="none";
        stbClear();
        uiAnchors();
        var h3=scrDead.querySelector("h3");
        var p=scrDead.querySelector("p");
        if(h3){ h3.textContent="No machines yet"; }
        if(p){ p.textContent="Create your first machine with New VM."; }
      }
      return VMS;
    });
  }
  function startPolling(){
    if(pollTimer){ return; }
    pollTimer=setInterval(function(){
      refreshVms().catch(function(){});
    },1500);
  }
  function loadUiState(){
    return api("/api/ui",{cache:"no-store"}).then(function(st){
      UI_STATE=st||{};
      document.documentElement.classList.toggle("dark",UI_STATE.theme==="dark");
      if(UI_STATE.agents&&UI_STATE.agents.length&&mkAgItems.length){
        mkAgItems.forEach(function(it){
          it.setAttribute("data-chk",UI_STATE.agents.indexOf(it.dataset.id)>=0?"true":"false");
        });
        mkAgVal.textContent=agSummary();
      }
      return UI_STATE;
    });
  }
  function boot(){
    var wanted="";
    try{ wanted=new URLSearchParams(location.search).get("id")||""; }catch(err){}
    if(wanted){
      selectedId=wanted;
    }else{
      try{ selectedId=localStorage.getItem("sparky-selected")||""; }catch(err){}
    }
    loadUiState().then(function(st){
      if(!st||st.onboardingComplete===false){
        location.replace("connect-github.html");
        return false;
      }
      return refreshVms().then(function(){
        startPolling();
        return true;
      });
    }).catch(function(){
      refreshVms().then(startPolling).catch(function(){});
    });
  }

  var inputKeys={};
  var inputButtons=0;
  var inputMoveX=0;
  var inputMoveY=0;
  var inputQueued=false;
  function inputLive(){
    return !!(sel&&sel.status==="running");
  }
  function sendInput(message){
    if(!inputLive()){ return; }
    fetch("/api/vms/"+encodeURIComponent(sel.id)+"/input",{
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify(message)
    }).catch(function(){});
  }
  function releaseInput(){
    Object.keys(inputKeys).forEach(function(k){
      sendInput({type:"key",vk:+k,down:false});
    });
    inputKeys={};
    inputButtons=0;
    if(inputLive()){ sendInput({type:"release"}); }
  }
  function flushInputMove(){
    inputQueued=false;
    var dx=Math.round(inputMoveX);
    var dy=Math.round(inputMoveY);
    inputMoveX-=dx;
    inputMoveY-=dy;
    if(!dx&&!dy){ return; }
    sendInput({type:"move",dx:dx,dy:dy,buttons:inputButtons});
  }
  function queueInputMove(dx,dy){
    inputMoveX+=dx;
    inputMoveY+=dy;
    if(!inputQueued){
      inputQueued=true;
      requestAnimationFrame(flushInputMove);
    }
  }
  function contentScale(){
    var rect=term.getBoundingClientRect();
    return Math.min(rect.width/1360,rect.height/768);
  }
  term.addEventListener("click",function(){
    if(!inputLive()){ return; }
    if(document.pointerLockElement!==term){
      var request=term.requestPointerLock&&term.requestPointerLock();
      if(request&&typeof request.catch==="function"){ request.catch(function(){}); }
    }
  });
  term.addEventListener("contextmenu",function(e){
    if(inputLive()){ e.preventDefault(); }
  });
  document.addEventListener("pointerlockchange",function(){
    var locked=document.pointerLockElement===term;
    term.classList.toggle("locked",locked);
    if(!locked){ releaseInput(); }
  });
  document.addEventListener("mousemove",function(e){
    if(document.pointerLockElement!==term||!inputLive()){ return; }
    var scale=contentScale();
    if(scale<=0){ return; }
    queueInputMove(e.movementX/scale,-e.movementY/scale);
  });
  document.addEventListener("mousedown",function(e){
    if(document.pointerLockElement!==term||!inputLive()){ return; }
    var bit=e.button===0?1:e.button===2?2:e.button===1?4:0;
    if(!bit){ return; }
    inputButtons|=bit;
    sendInput({type:"button",buttons:inputButtons});
    e.preventDefault();
  });
  document.addEventListener("mouseup",function(e){
    if(document.pointerLockElement!==term){ return; }
    var bit=e.button===0?1:e.button===2?2:e.button===1?4:0;
    if(!bit){ return; }
    inputButtons&=~bit;
    sendInput({type:"button",buttons:inputButtons});
    e.preventDefault();
  });
  document.addEventListener("wheel",function(e){
    if(document.pointerLockElement===term){ e.preventDefault(); }
  },{passive:false});
  document.addEventListener("keydown",function(e){
    if(document.pointerLockElement!==term||!inputLive()){ return; }
    if(e.key==="Escape"){ return; }
    inputKeys[e.keyCode]=true;
    sendInput({type:"key",vk:e.keyCode,down:true});
    e.preventDefault();
  });
  document.addEventListener("keyup",function(e){
    if(document.pointerLockElement!==term){ return; }
    delete inputKeys[e.keyCode];
    sendInput({type:"key",vk:e.keyCode,down:false});
    e.preventDefault();
  });
  window.addEventListener("blur",releaseInput);

  deleteBtn.addEventListener("click",function(){
    if(!sel||busy){ return; }
    var id=sel.id;
    if(!window.confirm('Delete "'+sel.title+'" and its VM data? This cannot be undone.')){ return; }
    busy=true;
    api("/api/vms/"+encodeURIComponent(id),{method:"DELETE"}).then(function(){
      selectedId="";
      return refreshVms();
    }).catch(function(err){
      alert(err.message||"Could not delete the machine.");
    }).then(function(){
      busy=false;
    });
  });

  newBtn.addEventListener("click",function(){
    mkScrim.classList.add("on");
    mkScrim.setAttribute("aria-hidden","false");
    mkName.value="";
    if(mkType==="apps"){ loadRepos(); }
    setTimeout(function(){ mkName.focus(); },60);
  });
  mkTrig.addEventListener("click",function(e){
    e.stopPropagation();
    var open=!mkDd.classList.contains("open");
    mkDd.classList.toggle("open",open);
    mkList.hidden=!open;
    mkTrig.setAttribute("aria-expanded",open?"true":"false");
  });
  mkRepoTrig.addEventListener("click",function(e){
    e.stopPropagation();
    var open=!mkRepoDd.classList.contains("open");
    mkRepoDd.classList.toggle("open",open);
    mkRepoList.hidden=!open;
    mkRepoTrig.setAttribute("aria-expanded",open?"true":"false");
  });
  function chooseRepo(opt){
    Array.prototype.forEach.call(mkRepoList.children,function(o){
      if(o===opt){ o.setAttribute("data-on",""); } else { o.removeAttribute("data-on"); }
    });
    mkRepo=opt.dataset.repo||"";
    mkRepoUrl=opt.dataset.url||"";
    mkRepoVal.querySelector("i").textContent=mkRepo;
    mkRepoDd.classList.remove("open");
    mkRepoList.hidden=true;
    mkRepoTrig.setAttribute("aria-expanded","false");
  }
  function addManualRepoOption(){
    var opt=document.createElement("button");
    opt.type="button";
    opt.className="mk-opt";
    opt.innerHTML='<span>'+svg24(gitPath,' class="mk-git"')+"Use a public repository URL...</span>";
    opt.addEventListener("click",function(e){
      e.stopPropagation();
      var url=window.prompt("Public GitHub repository URL","https://github.com/");
      if(!url||!url.trim()){ return; }
      var parsed=parseRepo(url.trim());
      if(!parsed){
        mkRepoVal.querySelector("i").textContent="Enter a GitHub repository URL";
        return;
      }
      opt.dataset.repo=parsed.owner+"/"+parsed.repo;
      opt.dataset.url="https://github.com/"+parsed.owner+"/"+parsed.repo;
      chooseRepo(opt);
    });
    mkRepoList.appendChild(opt);
  }
  function loadRepos(){
    mkRepo="";
    mkRepoUrl="";
    mkRepoVal.querySelector("i").textContent="Loading repositories...";
    mkRepoList.innerHTML='<button class="mk-opt" type="button" disabled><span>Loading repositories...</span></button>';
    api("/api/github/repos",{cache:"no-store"}).then(function(data){
      var repos=(data&&data.repos)||[];
      mkRepoList.innerHTML="";
      repos.forEach(function(repo){
        var label=repo.fullName||repo.name||"repository";
        var url=repo.htmlUrl||("https://github.com/"+label);
        var opt=document.createElement("button");
        opt.type="button";
        opt.className="mk-opt";
        opt.dataset.repo=label;
        opt.dataset.url=url;
        opt.innerHTML='<span>'+svg24(gitPath,' class="mk-git"')+h(label)+(repo.private?" (private)":"")+"</span>";
        mkRepoList.appendChild(opt);
        if(mkRepoList.children.length===1){ chooseRepo(opt); }
      });
      if(!repos.length){
        mkRepoVal.querySelector("i").textContent="No repositories";
      }
      addManualRepoOption();
    }).catch(function(){
      mkRepoList.innerHTML="";
      var connect=document.createElement("button");
      connect.type="button";
      connect.className="mk-opt";
      connect.textContent="Connect GitHub...";
      connect.addEventListener("click",function(){
        mkRepoDd.classList.remove("open");
        mkRepoList.hidden=true;
        if(window.SoilGitHub){ window.SoilGitHub.open(); }
      });
      mkRepoList.appendChild(connect);
      addManualRepoOption();
      mkRepoVal.querySelector("i").textContent="Connect GitHub or use a public URL";
    });
  }
  window.addEventListener("soil:github-connected",function(){
    if(mkScrim.classList.contains("on")&&mkType==="apps"){ loadRepos(); }
  });
  mkRepoList.addEventListener("click",function(e){
    var opt=e.target.closest(".mk-opt");
    if(!opt||opt.disabled){ return; }
    e.stopPropagation();
    chooseRepo(opt);
  });
  mkList.addEventListener("click",function(e){
    var opt=e.target.closest(".mk-opt");
    if(!opt){ return; }
    Array.prototype.forEach.call(mkList.children,function(o){
      if(o===opt){ o.setAttribute("data-on",""); } else { o.removeAttribute("data-on"); }
    });
    mkPkg={cpu:+opt.dataset.cpu,mem:+opt.dataset.mem,label:opt.querySelector("span").textContent,size:sizeForMem(+opt.dataset.mem)};
    mkPkgVal.textContent=mkPkg.label;
    mkDd.classList.remove("open");
    mkList.hidden=true;
    mkTrig.setAttribute("aria-expanded","false");
  });
  var mkType="lab";
  var mkAgItems=[];
  ["codex","claude","antigravity","sparky"].forEach(function(id){
    var it=document.createElement("button");
    it.type="button";
    it.className="mk-opt mk-ag-opt";
    it.setAttribute("data-chk","true");
    it.dataset.id=id;
    it.innerHTML='<span>'+agentIconHtml(id)+"</span><span>"+AGENTS[id]+"</span>"+'<span class="mk-ck"><svg viewBox="0 0 20 20" aria-hidden="true"><path d="M4 10.5l4 4 8-9"/></svg></span>';
    mkAgList.appendChild(it);
    mkAgItems.push(it);
  });
  function agSummary(){
    var names=[],i;
    for(i=0;i<mkAgItems.length;i++){
      if(mkAgItems[i].getAttribute("data-chk")==="true"){ names.push(AGENTS[mkAgItems[i].dataset.id]); }
    }
    if(names.length===mkAgItems.length){ return "All agents ("+names.length+")"; }
    if(names.length===0){ return "No agents selected"; }
    if(names.length<=2){ return names.join(", "); }
    return names[0]+" +"+(names.length-1)+" more";
  }
  function openType(){
    mkTypeDd.classList.add("open");
    mkTypeList.hidden=false;
    mkTypeTrig.setAttribute("aria-expanded","true");
  }
  function closeType(){
    mkTypeDd.classList.remove("open");
    mkTypeList.hidden=true;
    mkTypeTrig.setAttribute("aria-expanded","false");
  }
  function openAg(){
    mkAgDd.classList.add("open");
    mkAgList.hidden=false;
    mkAgTrig.setAttribute("aria-expanded","true");
  }
  function closeAg(){
    mkAgDd.classList.remove("open");
    mkAgList.hidden=true;
    mkAgTrig.setAttribute("aria-expanded","false");
  }
  mkTypeTrig.addEventListener("click",function(){
    if(mkTypeDd.classList.contains("open")){ closeType(); } else { openType(); }
  });
  mkAgTrig.addEventListener("click",function(){
    if(mkAgDd.classList.contains("open")){ closeAg(); } else { openAg(); }
  });
  mkTypeList.addEventListener("click",function(e){
    var opt=e.target.closest(".mk-opt");
    if(!opt){ return; }
    mkType=opt.dataset.type;
    Array.prototype.forEach.call(mkTypeList.children,function(o){
      if(o===opt){ o.setAttribute("data-on",""); } else { o.removeAttribute("data-on"); }
    });
    mkTypeVal.textContent=opt.textContent;
    closeType();
    mkRepoWrap.hidden=mkType!=="apps";
    mkAgWrap.hidden=mkType!=="agents";
    if(mkType==="apps"){ loadRepos(); }
  });
  mkAgList.addEventListener("click",function(e){
    var opt=e.target.closest(".mk-ag-opt");
    if(!opt){ return; }
    opt.setAttribute("data-chk",opt.getAttribute("data-chk")==="true"?"false":"true");
    mkAgVal.textContent=agSummary();
  });
  document.addEventListener("click",function(e){
    if(mkDd.classList.contains("open")&&!mkDd.contains(e.target)){
      mkDd.classList.remove("open");
      mkList.hidden=true;
      mkTrig.setAttribute("aria-expanded","false");
    }
    if(mkRepoDd.classList.contains("open")&&!mkRepoDd.contains(e.target)){
      mkRepoDd.classList.remove("open");
      mkRepoList.hidden=true;
      mkRepoTrig.setAttribute("aria-expanded","false");
    }
    if(mkTypeDd.classList.contains("open")&&!mkTypeDd.contains(e.target)){
      mkTypeDd.classList.remove("open");
      mkTypeList.hidden=true;
      mkTypeTrig.setAttribute("aria-expanded","false");
    }
    if(mkAgDd.classList.contains("open")&&!mkAgDd.contains(e.target)){
      mkAgDd.classList.remove("open");
      mkAgList.hidden=true;
      mkAgTrig.setAttribute("aria-expanded","false");
    }
  });
  mkName.addEventListener("keydown",function(e){
    if(e.key==="Enter"){ mkDone.click(); }
  });
  function mkClose(){
    mkScrim.classList.remove("on");
    mkScrim.setAttribute("aria-hidden","true");
    mkDd.classList.remove("open");
    mkList.hidden=true;
    mkTrig.setAttribute("aria-expanded","false");
    mkRepoDd.classList.remove("open");
    mkRepoList.hidden=true;
    mkRepoTrig.setAttribute("aria-expanded","false");
    closeType();
    closeAg();
  }
  mkScrim.addEventListener("click",function(e){
    if(e.target===mkScrim){ mkClose(); }
  });
  document.addEventListener("keydown",function(e){
    if(e.key==="Escape"&&mkScrim.classList.contains("on")){ mkClose(); }
  });
  function parseRepo(url){
    var m=String(url||"").match(/github\.com[/:]([^/]+)\/([^/#?]+)/i);
    if(!m){ return null; }
    return {owner:m[1],repo:m[2].replace(/\.git$/i,"")};
  }
  mkDone.addEventListener("click",function(){
    if(busy){ return; }
    var raw=mkName.value.trim().toLowerCase().replace(/[^a-z0-9-]+/g,"-").replace(/^-+|-+$/g,"")||"machine";
    var agOn=[],i;
    for(i=0;i<mkAgItems.length;i++){
      if(mkAgItems[i].getAttribute("data-chk")==="true"){ agOn.push(mkAgItems[i].dataset.id); }
    }
    if(mkType==="agents"&&!agOn.length){
      mkAgVal.textContent="Pick at least one agent";
      mkAgTrig.classList.remove("shake");
      void mkAgTrig.offsetWidth;
      mkAgTrig.classList.add("shake");
      openAg();
      return;
    }
    var repo=parseRepo(mkRepoUrl||mkRepo);
    if(mkType==="apps"&&!repo){
      mkRepoVal.querySelector("i").textContent="Pick a repository first";
      mkRepoTrig.classList.remove("shake");
      void mkRepoTrig.offsetWidth;
      mkRepoTrig.classList.add("shake");
      mkRepoDd.classList.add("open");
      mkRepoList.hidden=false;
      mkRepoTrig.setAttribute("aria-expanded","true");
      loadRepos();
      return;
    }
    busy=true;
    mkDone.disabled=true;
    if(mkType==="agents"&&agOn.length){
      if(UI_STATE){ UI_STATE.agents=agOn; }
      postJson("/api/ui",{agents:agOn}).catch(function(){});
    }
    var request;
    if(mkType==="apps"){
      request=postJson("/api/apps",{
        name:raw,
        repoUrl:mkRepoUrl,
        owner:repo.owner,
        repo:repo.repo,
        ref:null,
        size:mkPkg.size
      });
    }else if(mkType==="agents"){
      request=postJson("/api/agents",{name:raw});
    }else{
      request=postJson("/api/vms",{name:raw,size:mkPkg.size});
    }
    request.then(function(dto){
      mkClose();
      selectedId=dto.id;
      // New machines start only when the user explicitly presses Start.
      return null;
    }).then(function(){
      return refreshVms();
    }).catch(function(err){
      alert(err.message||"Could not create the machine.");
    }).then(function(){
      busy=false;
      mkDone.disabled=false;
    });
  });

  var tour=document.getElementById("tour");
  var tourScrim=document.getElementById("tourScrim");
  var tourRing=document.getElementById("tourRing");
  var tourCard=document.getElementById("tourCard");
  var tourK=document.getElementById("tourK");
  var tourT=document.getElementById("tourT");
  var tourD=document.getElementById("tourD");
  var tourDots=document.getElementById("tourDots");
  var tourBack=document.getElementById("tourBack");
  var tourSkip=document.getElementById("tourSkip");
  var tourNext=document.getElementById("tourNext");
  var TOUR=[
    {sel:"#projs",t:"Your machines",d:"Every machine you create lives in this list. Click one to open it.",c:"#4b7cff"},
    {sel:"#newVm",t:"New machine",d:"Spin up a fresh machine, or attach one of your repos, in a couple of clicks.",c:"#49a2ff"},
    {sel:"#startBtn",t:"Screen & power",d:"The live screen and power controls land here on the right. Hit Start to boot.",c:"#e85b83"},
    {sel:"#stbPlot",t:"Live resources",d:"CPU and memory samples from the selected machine, refreshed every couple of seconds.",c:"#f2c34c"},
    {sel:"#themeTgl",t:"Themes",d:"Light and dark modes. Your pick is remembered across visits.",c:"#4b7cff"}
  ];
  var tourI=-1;
  TOUR.forEach(function(){ tourDots.appendChild(document.createElement("i")); });
  function tourFit(){
    var cur=TOUR[tourI];
    if(!cur){ return; }
    var el=document.querySelector(cur.sel);
    if(!el){ tourStep(tourI+1); return; }
    var r=el.getBoundingClientRect(), p=8, M=12, GAP=22;
    var L=Math.max(0,r.left-p), T=Math.max(0,r.top-p);
    var R=Math.min(window.innerWidth,r.right+p), B=Math.min(window.innerHeight,r.bottom+p);
    tourRing.style.left=L.toFixed(0)+"px";
    tourRing.style.top=T.toFixed(0)+"px";
    tourRing.style.width=(R-L).toFixed(0)+"px";
    tourRing.style.height=(B-T).toFixed(0)+"px";
    var cw=tourCard.offsetWidth||316, ch=tourCard.offsetHeight||180;
    var dir, cx, cy;
    if(B+GAP+ch<=window.innerHeight-M){ dir="down"; cy=B+GAP; cx=r.left+r.width/2-cw/2; }
    else if(T-GAP-ch>=M){ dir="up"; cy=T-GAP-ch; cx=r.left+r.width/2-cw/2; }
    else if(R+GAP+cw<=window.innerWidth-M){ dir="right"; cx=R+GAP; cy=r.top+r.height/2-ch/2; }
    else if(L-GAP-cw>=M){ dir="left"; cx=L-GAP-cw; cy=r.top+r.height/2-ch/2; }
    else { dir="down"; cy=B+GAP; cx=r.left+r.width/2-cw/2; }
    cx=Math.min(window.innerWidth-cw-M,Math.max(M,cx));
    cy=Math.min(window.innerHeight-ch-M,Math.max(M,cy));
    tourCard.setAttribute("data-dir",dir);
    tourCard.style.left=cx.toFixed(0)+"px";
    tourCard.style.top=cy.toFixed(0)+"px";
  }
  function tourStep(i){
    if(i<0||i>=TOUR.length){ tourEnd(); return; }
    tourI=i;
    var t=TOUR[i];
    tourK.textContent="Step "+(i+1)+" of "+TOUR.length;
    tourT.textContent=t.t;
    tourD.textContent=t.d;
    tourCard.style.setProperty("--tc",t.c);
    Array.prototype.forEach.call(tourDots.children,function(d,j){
      if(j===i){ d.setAttribute("data-on",""); } else { d.removeAttribute("data-on"); }
    });
    tourNext.textContent=(i===TOUR.length-1)?"Done":"Next";
    tourBack.style.display=i===0?"none":"inline-flex";
    tour.classList.add("on");
    tourCard.classList.remove("popin");
    void tourCard.offsetWidth;
    tourCard.classList.add("popin");
    requestAnimationFrame(function(){ requestAnimationFrame(tourFit); });
  }
  function tourEnd(){
    if(tourI<0||tour.classList.contains("off")){ return; }
    tourI=-1;
    tour.classList.add("off");
    try{ sessionStorage.setItem("sparky-tour-done","1"); }catch(err){}
    setTimeout(function(){
      tour.classList.remove("on","off","popin");
    },320);
  }
  tourNext.addEventListener("click",function(){
    if(tourI>=TOUR.length-1){ tourEnd(); } else { tourStep(tourI+1); }
  });
  tourBack.addEventListener("click",function(){ tourStep(tourI-1); });
  tourSkip.addEventListener("click",tourEnd);
  window.addEventListener("keydown",function(e){
    if(tourI<0){ return; }
    if(e.key==="Escape"){ tourEnd(); }
    else if(e.key==="ArrowRight"){ e.preventDefault(); if(tourI<TOUR.length-1){ tourStep(tourI+1); } }
    else if(e.key==="ArrowLeft"){ e.preventDefault(); if(tourI>0){ tourStep(tourI-1); } }
  });
  window.addEventListener("resize",function(){
    if(tourI>=0){ tourFit(); }
  });
  var tourSeen=false;
  try{ tourSeen=!!sessionStorage.getItem("sparky-tour-done"); }catch(err){}
  if(!tourSeen||/tour/.test(location.hash)){ setTimeout(tourStart,1300); }
  function tourStart(){ if(tourI<0){ tourStep(0); } }

  boot();
  var themeTgl=document.getElementById("themeTgl");
  function setTheme(dark){
    document.documentElement.classList.toggle("dark",dark);
    try{ localStorage.setItem("sparky-theme",dark?"dark":"light"); }catch(err){}
    if(UI_STATE){
      UI_STATE.theme=dark?"dark":"light";
      postJson("/api/ui",{theme:UI_STATE.theme}).catch(function(){});
    }
  }
  themeTgl.addEventListener("click",function(){
    var dark=!document.documentElement.classList.contains("dark");
    var vt=typeof document.startViewTransition==="function"&&!reduced;
    if(vt){
      var r=themeTgl.getBoundingClientRect();
      var cx=r.left+r.width/2, cy=r.top+r.height/2;
      var rad=Math.hypot(Math.max(cx,window.innerWidth-cx),Math.max(cy,window.innerHeight-cy))+4;
      var st=document.documentElement.style;
      st.setProperty("--wx",cx+"px");
      st.setProperty("--wy",cy+"px");
      st.setProperty("--wr",rad+"px");
      document.documentElement.classList.add("vt");
      var trans=document.startViewTransition(function(){ setTheme(dark); });
      function fin(){
        document.documentElement.classList.remove("vt");
      }
      trans.finished.then(fin).catch(fin);
    } else {
      setTheme(dark);
    }
  });
  requestAnimationFrame(function(){
    requestAnimationFrame(function(){
      document.body.classList.add("settled");
    });
  });
})();
