import re,sys,json
vals={'zh_TW':('桌寵','觸控鍵盤')}
for loc,(pet,kb) in vals.items():
    p='public/assets/locale/%s.json'%loc
    lines=open(p,encoding='utf8').read().split('\n')
    start=lines.index('  "apps": {')
    i=start+1
    while True:
        m=re.match(r'    "([^"]+)":',lines[i])
        if m and m.group(1)>'default_menu_cmd': break
        i+=1
    new=['    "default_menu_cmd_desktop_pet": %s,'%json.dumps(pet,ensure_ascii=False),
         '    "default_menu_cmd_touch_keyboard": %s,'%json.dumps(kb,ensure_ascii=False)]
    lines[i:i]=new
    open(p,'w',encoding='utf8',newline='').write('\n'.join(lines))
    json.load(open(p,encoding='utf8'))
    pass
