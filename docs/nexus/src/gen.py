import json, html
FONT = """
@font-face{font-family:T;src:url(tahoma.ttf)}
@font-face{font-family:T;font-weight:bold;src:url(tahomabd.ttf)}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:T,sans-serif;background:#008080;color:#000;overflow:hidden}
.win{background:#c0c0c0;border:2px solid;border-color:#fff #000 #000 #fff;box-shadow:inset -2px -2px #808080,inset 2px 2px #dfdfdf;padding:4px}
.title{background:linear-gradient(90deg,#000080,#1084d0);color:#fff;font-weight:bold;display:flex;align-items:center;justify-content:space-between;padding:4px 6px}
.title img{image-rendering:pixelated}
.btns{display:flex;gap:3px}
.btn{width:30px;height:26px;background:#c0c0c0;border:2px solid;border-color:#fff #000 #000 #fff;box-shadow:inset -1px -1px #808080;color:#000;font-weight:bold;display:flex;align-items:center;justify-content:center;font-size:18px;line-height:1}
.taskbar{position:absolute;left:0;right:0;bottom:0;height:56px;background:#c0c0c0;border-top:2px solid #fff;display:flex;align-items:center;gap:8px;padding:0 6px}
.start{height:42px;padding:0 14px;display:flex;align-items:center;gap:8px;font-weight:bold;font-size:22px;border:2px solid;border-color:#fff #000 #000 #fff;box-shadow:inset -2px -2px #808080}
.task{height:42px;padding:0 14px;display:flex;align-items:center;gap:8px;font-size:20px;border:2px solid;border-color:#000 #fff #fff #000;background:#dcdcdc;min-width:300px}
.clock{margin-left:auto;height:42px;padding:0 16px;display:flex;align-items:center;font-size:20px;border:2px solid;border-color:#808080 #fff #fff #808080}
"""
def taskbar(task="KETAMINE++"):
    return f"""<div class="taskbar"><div class="start"><img src="icon_64.png" width="28" style="image-rendering:pixelated">Start</div>
<div class="task"><img src="icon_64.png" width="24" style="image-rendering:pixelated">{task}</div><div class="clock">12:00 AM</div></div>"""

def card(fname, tab, headline, bullets, shot, extra=""):
    lis="".join(f"<li>{b}</li>" for b in bullets)
    return f"""<html><head><style>{FONT}
.info{{position:absolute;left:70px;top:150px;width:700px}}
.info .body{{background:#fff;border:2px solid;border-color:#808080 #fff #fff #808080;margin:6px 2px 2px;padding:28px 34px 34px}}
.tab{{font-size:28px;color:#000080;font-weight:bold}}
h1{{font-size:68px;line-height:1.05;margin:6px 0 22px}}
ul{{list-style:none;font-size:29px;line-height:1.55}}
li:before{{content:"▸ ";color:#000080}}
.shot{{position:absolute;right:70px;top:120px}}
.shot img{{display:block;width:1000px;box-shadow:10px 10px 0 rgba(0,0,0,.45)}}
</style></head><body style="width:1920px;height:1080px;position:relative">
<div class="win info"><div class="title" style="font-size:24px"><span style="display:flex;gap:8px;align-items:center"><img src="icon_64.png" width="26">{tab}.txt - Notepad</span><span class="btns"><span class="btn">_</span><span class="btn">□</span><span class="btn">×</span></span></div>
<div class="body"><div class="tab">{tab}</div><h1>{headline}</h1><ul>{lis}</ul>{extra}</div></div>
<div class="shot">{shot}</div>
{taskbar()}</body></html>"""

cards = [
 ("card-01-player","Player","Untouchable",["God mode &amp; infinite health","Set your health to any value","No sprint cooldown, invisible to enemies","One-hit kill &amp; damage multiplier"],"player.png"),
 ("card-02-combat","Combat","Every shot lands",["Silent aim with an adjustable circle","Infinite ammo, unbreakable melee","Genesis Configuration powers: no cost, no cooldown","Unlock Telekinesis, Pyrokinesis, Hellchains early"],"combat.png"),
 ("card-03-movement","Movement","Jump. Hop. Fly.",["A real jump key, bunny hop with momentum","Manual timing: perfect hops gain double","Speedometer in km/h, mph, hammer or Unreal units","Look straight up and down, fly, noclip, free camera"],"movement.png"),
 ("card-04-fov","FOV editor","TrueFOV, built in",["World, viewmodel and aiming FOV, live","Keeps the game's sprint and ladder effects","Page Up / Page Down to step, End to toggle","Based on TrueFOV by Reynbow"],"fov.png"),
 ("card-05-minimap","Minimap","True Minimap, no stutter",["Square, circle, octagon or diamond","Rotates with you or stays north-up","Frame in your skin's accent colour","Based on True Minimap by Thidrec, rebuilt to be light"],"minimap.png"),
 ("card-06-fixes","Fixes","Five fixes, no UE4SS",["Keep movement after tools, keypad input fix","Speed up save points, puzzles, pillars, boat","Quick interact, auto-skip cutscenes","Ports of uwumatic101's mods, native"],"fixes.png"),
 ("card-07-world","World","Your rules",["Time scale, freeze or clear enemies","Simple prompts: just the key, [F], minimal","Hide the HUD, or only its idle leftovers","ESP for enemies, items, containers, save points"],"world.png"),
]
out={}
for f,tab,h,b,img in cards:
    out[f]=card(f,tab,h,b,f'<img src="{img}">')
# skins card: grid
grid="".join(f'<div style="text-align:center"><img src="{i}" style="width:470px;display:block;box-shadow:8px 8px 0 rgba(0,0,0,.45)"><div style="color:#fff;font-size:24px;margin-top:10px;font-weight:bold;text-shadow:2px 2px #000">{n}</div></div>'
    for i,n in [("settings.png","Win95"),("skin_blackhat.png","Black Hat"),("skin_phosphor.png","Phosphor"),("skin_netrunner.png","Netrunner"),("skin_bsod.png","BSOD"),("skin_ketamine.png","KETAMINE++")])
out["card-08-skins"]=f"""<html><head><style>{FONT}</style></head><body style="width:1920px;height:1080px;position:relative">
<div style="position:absolute;left:0;right:0;top:40px;text-align:center;color:#fff;font-weight:bold;font-size:64px;text-shadow:3px 3px #000">18 skins + your own: retro, terminal, modern</div>
<div style="position:absolute;left:150px;right:150px;top:150px;display:grid;grid-template-columns:repeat(3,1fr);gap:34px 40px">{grid}</div>
{taskbar()}</body></html>"""
# banner
out["banner"]=f"""<html><head><style>{FONT}</style></head><body style="width:1920px;height:527px;position:relative">
<img src="icon_512.png" style="position:absolute;left:150px;top:70px;width:384px;image-rendering:pixelated;filter:drop-shadow(10px 10px 0 rgba(0,0,0,.4))">
<div class="win" style="position:absolute;left:640px;top:70px;width:1130px">
<div class="title" style="font-size:26px"><span style="display:flex;gap:10px;align-items:center"><img src="icon_64.png" width="28">KETAMINE++.exe</span><span class="btns"><span class="btn">_</span><span class="btn">□</span><span class="btn">×</span></span></div>
<div style="padding:26px 34px 30px"><div style="font-size:112px;font-weight:bold;letter-spacing:2px;line-height:1">KETAMINE++</div>
<div style="font-size:31px;margin-top:16px">Trainer, FOV editor, minimap, game fixes, jump &amp; bunny hop.</div>
<div style="font-size:31px">One native DLL for Clive Barker's Hellraiser: Revival. No UE4SS.</div>
<div style="font-size:22px;color:#404040;margin-top:14px">by rams95 &nbsp;·&nbsp; built on Ketamine Menu (omxga), TrueFOV (Reynbow), uwumatic101's fixes, True Minimap (Thidrec)</div></div></div>
</body></html>"""
# logo / main image (16:9)
out["logo"]=f"""<html><head><style>{FONT}</style></head><body style="width:1920px;height:1080px;position:relative">
<div class="win" style="position:absolute;left:310px;top:250px;width:1300px">
<div class="title" style="font-size:30px"><span style="display:flex;gap:10px;align-items:center"><img src="icon_64.png" width="32">KETAMINE++</span><span class="btns"><span class="btn">_</span><span class="btn">□</span><span class="btn">×</span></span></div>
<div style="display:flex;align-items:center;gap:50px;padding:50px 60px">
<img src="icon_512.png" style="width:320px;image-rendering:pixelated">
<div><div style="font-size:104px;font-weight:bold;line-height:1">KETAMINE++</div>
<div style="font-size:36px;margin-top:20px">All-in-one native trainer &amp; fixes</div><div style="font-size:36px">for Hellraiser: Revival</div></div></div></div>
{taskbar()}</body></html>"""
for k,v in out.items(): open(f"{k}.html","w").write(v)
print(list(out))
