import re,glob,sys,os
sys.stdout.reconfigure(encoding='utf-8')
for p in sys.argv[1:]:
  for i,l in enumerate(open(p,encoding='utf8'),1):
    if re.search(r'"[^"\n]*[一-鿿][^"\n]*"',l) and not re.search(r'Log\.|LimeLog|^\s*(//|\*)',l):
      print(os.path.basename(p),i,l.strip()[:110])
