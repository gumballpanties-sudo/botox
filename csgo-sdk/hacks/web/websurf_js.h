#pragma once

static const char* k_wsSmoothJs = R"JS((function(){try{
if(window.__fpsQ)return;window.__fpsQ=1;
var H=[144,240,360,480,720,1080,1440,2160],N=[0.15,0.3,0.7,1.2,2.5,4.5,9,18];
var YT=['tiny','small','medium','large','hd720','hd1080','hd1440','hd2160'];
var pen=0,lastBad=0,lastUp=Date.now(),lastApply=0,applied='',lvl=-1;
var yt=/(^|\.)youtube(-nocookie)?\.com$/.test(location.hostname),tw=/(^|\.)twitch\.tv$/.test(location.hostname);
var bw=function(){var n=0,s=0;try{performance.getEntriesByType('resource').slice(-80).forEach(function(e){
  var d=e.responseEnd-e.requestStart;if(e.transferSize>150000&&d>5){s+=e.transferSize*8/(d*1000);n++;}});}catch(x){}
  if(n>=2)return s/n;if(window.__fpsBw>0)return window.__fpsBw;
  var c=navigator.connection;return c&&c.downlink?c.downlink:5;};
var speedCap=function(){var b=bw(),i=0;for(var k=0;k<H.length;k++)if(N[k]*1.6<=b)i=k;return i;};
var viewCap=function(side){var i=0;while(i<H.length-1&&H[i]<side*0.95)i++;return i;};
var bad=function(){var n=Date.now();if(n-lastApply<6000||n-lastBad<4000)return;pen++;lastBad=n;};
var pref=function(i){try{
  if(yt){var t=Date.now();localStorage.setItem('yt-player-quality',JSON.stringify({data:JSON.stringify({quality:H[i],previousQuality:H[i]}),expiration:t+2592e6,creation:t}));}
  if(tw)localStorage.setItem('video-quality',JSON.stringify({'default':H[i]+'p30'}));
}catch(x){}};
pref(Math.min(viewCap(Math.min(innerWidth,innerHeight)||720),speedCap()));
var apply=function(i){
  if(i!==lvl){lvl=i;pref(i);}
  if(!yt)return;
  var p=document.getElementById('movie_player');if(!p||!p.getAvailableQualityLevels)return;
  var av=p.getAvailableQualityLevels()||[],want='';
  for(var k=i;k>=0&&!want;k--)if(av.indexOf(YT[k])>=0)want=YT[k];
  if(!want)return;
  if((want!==applied||p.getPlaybackQuality()!==want)&&Date.now()-lastApply>5000){
    try{p.setPlaybackQualityRange(want,want);}catch(x){}applied=want;lastApply=Date.now();}
};
setInterval(function(){try{
  if(window.__fpsQOff)return;
  var vs=[].slice.call(document.querySelectorAll('video')),v=null,a=0;
  vs.forEach(function(e){var r=e.getBoundingClientRect(),s=r.width*r.height;if(s>a){a=s;v=e;}});
  if(!v)return;
  if(!v.__fpsQ){v.__fpsQ=1;var pl=false;v.addEventListener('playing',function(){pl=true;});
    v.addEventListener('waiting',function(){if(pl&&!v.seeking&&v.currentTime>1)bad();});
    var q0=v.getVideoPlaybackQuality&&v.getVideoPlaybackQuality();v.__fpsT=q0?q0.totalVideoFrames:0;v.__fpsD=q0?q0.droppedVideoFrames:0;}
  var q=v.getVideoPlaybackQuality&&v.getVideoPlaybackQuality();
  if(q&&!v.paused){var dt=q.totalVideoFrames-v.__fpsT,dd=q.droppedVideoFrames-v.__fpsD;
    if(dt>=120){if(dd/dt>0.1)bad();v.__fpsT=q.totalVideoFrames;v.__fpsD=q.droppedVideoFrames;}}
  var n=Date.now();if(pen>0&&n-lastBad>60000&&n-lastUp>60000){pen--;lastUp=n;}
  var r=v.getBoundingClientRect(),side=Math.min(r.width,r.height);
  if(side<16)return;
  apply(Math.max(0,Math.min(viewCap(side),speedCap())-pen));
  window.__fpsQState=H[Math.max(0,Math.min(viewCap(side),speedCap())-pen)]+'p pen='+pen+' bw='+bw().toFixed(1);
}catch(e){}},1000);
}catch(e){}})())JS";

#define WS_MAINVIDEO_JS \
    "var M=function(){var v=null,a=0;[].slice.call(document.querySelectorAll('video')).forEach(function(e){" \
    "var r=e.getBoundingClientRect(),s=r.width*r.height;if(s>a){a=s;v=e;}});return v;};"

static const char* k_wsPlayJs =
    "(function(){try{" WS_MAINVIDEO_JS
    "var from0=!!window.__fpsPlayFrom0;window.__fpsPlayFrom0=0;"
    "if(window.__fpsPlayT)clearInterval(window.__fpsPlayT);"
    "var n=0,sought=false;var go=function(){try{"
    "var v=M(),p=document.getElementById('movie_player');"
    "if(from0&&!sought&&v&&isFinite(v.duration)&&v.duration>0){sought=true;"
    "if(p&&p.seekTo)p.seekTo(0,true);else v.currentTime=0;}"
    "if(v&&!v.paused&&!v.ended&&v.readyState>2&&(!from0||sought||!isFinite(v.duration))){clearInterval(window.__fpsPlayT);return;}"
    "if(p&&p.playVideo)p.playVideo();"
    "if(v&&(v.paused||v.ended)){var r=v.play();if(r&&r.catch)r.catch(function(){});}"
    "}catch(e){}if(++n>=20)clearInterval(window.__fpsPlayT);};"
    "window.__fpsPlayT=setInterval(go,250);go();"
    "}catch(e){}})()";

static const char* k_wsPauseJs =
    "(function(){try{if(window.__fpsPlayT)clearInterval(window.__fpsPlayT);"
    "var p=document.getElementById('movie_player');if(p&&p.pauseVideo)p.pauseVideo();"
    "document.querySelectorAll('video,audio').forEach(function(e){try{e.pause();}catch(x){}});"
    "}catch(e){}})()";

static const char* k_wsEndedTag = "__fpsended";
static const char* k_wsEndedJs =
    "(function(){try{if(window.__fpsEnd)return;window.__fpsEnd=1;" WS_MAINVIDEO_JS
    "document.addEventListener('ended',function(e){try{var v=e.target;"
    "if(!v||v.tagName!=='VIDEO'||v!==M()||!(v.duration>3))return;"
    "var o=/^__fps/.test(document.title)?(window.__fpsT0||''):document.title;"
    "document.title='__fpsended';setTimeout(function(){document.title=o;},200);"
    "}catch(x){}},true);}catch(e){}})()";
