import sys
from pathlib import Path
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

source = Path(sys.argv[1]) if len(sys.argv)>1 else Path('estadisticas.csv')
base = Path(sys.argv[2]) if len(sys.argv)>2 else Path('Graficas_HQC')
d = pd.read_csv(source).rename(columns={'topologia':'topology','condicion':'condition','metrica':'metric','desv_estandar':'std'})
conds = ['C0','C2','C3','C4']
names = {'C0':'C0 - Base de red','C2':'C2 - HQC','C3':'C3 - Impacto de unión','C4':'C4 - Base temperatura'}
metrics = {
'radio_bytes':('Tráfico radio total','Bytes TX+RX [bytes]'),
'radio_tx_bytes':('Bytes de radio transmitidos','Bytes TX [bytes]'),
'radio_rx_bytes':('Bytes de radio recibidos','Bytes RX [bytes]'),
'app_bytes':('Bytes de aplicación','Bytes de aplicación [bytes]'),
'mac_extra_attempts':('Intentos MAC extra','Intentos MAC extra [intentos]'),
'retransmission_pressure':('Presión de retransmisión','Índice agregado [eventos]'),
'nack_retransmitted_fragments':('Fragmentos recuperados por NACK','Fragmentos [fragmentos]'),
'retransmitted_bytes':('Bytes retransmitidos','Bytes retransmitidos [bytes]'),
'hqc_total_ticks_max':('Tiempo máximo HQC','Tiempo HQC máximo [ticks]'),
'radio_transmit_ticks':('Tiempo de transmisión radio','Ticks TX radio [ticks]'),
'radio_listen_ticks':('Tiempo de escucha radio','Ticks escucha [ticks]'),
'temp_tx':('Transmisiones de temperatura','Temp TX [cuentas]')
}
for t in ['P2P','LINE','STAR','TREE','COMPARATIVA_TOPOLOGIAS']:
    (base/t).mkdir(parents=True,exist_ok=True)

def note_n(ax, xs, ys, ns):
    for x,y,n in zip(xs,ys,ns):
        if pd.notna(y):
            ax.annotate(f'n={int(n)}',(x,y),xytext=(0,6),textcoords='offset points',ha='center',fontsize=8)

def makefig(title,ylabel,xlabel='Número de nodos (N)'):
    fig,ax=plt.subplots(figsize=(10,6),dpi=180)
    ax.set(title=title,xlabel=xlabel,ylabel=ylabel)
    ax.grid(alpha=.3)
    return fig,ax

def save(fig,path,foot):
    fig.text(.01,.01,foot,fontsize=8)
    fig.tight_layout(rect=[0,.05,1,1])
    fig.savefig(path,bbox_inches='tight')
    plt.close(fig)

def series(top,metric,cond):
    return d[(d.topology==top)&(d.metric==metric)&(d.condition==cond)].sort_values('N')

def pairs(top,metric):
    a=series(top,metric,'C2')
    b=series(top,metric,'C4')
    return a[['N','media','n']].merge(b[['N','media','n']],on='N',suffixes=('_C2','_C4'))

for top in ['LINE','STAR','TREE']:
    for metric,(title,ylabel) in metrics.items():
        fig,ax=makefig(f'{top}: {title} por condición',ylabel)
        for c in conds:
            z=series(top,metric,c)
            if z.empty: continue
            ax.errorbar(z.N,z.media,yerr=z['std'].fillna(0),fmt='-o',capsize=4,label=names[c])
            note_n(ax,z.N,z.media,z.n)
        ax.legend(fontsize=8)
        save(fig,base/top/f'{top}_{metric}_por_condicion.png','Media ± desviación estándar; n indica ejecuciones seleccionadas. n<3 requiere cautela.')
    for metric,kind in [('radio_bytes','ratio'),('radio_bytes','delta'),('app_bytes','delta'),('mac_extra_attempts','delta'),('retransmission_pressure','delta'),('nack_retransmitted_fragments','delta'),('retransmitted_bytes','delta')]:
        z=pairs(top,metric)
        if z.empty: continue
        if kind=='ratio':
            z=z[z.media_C4!=0]
            y=z.media_C2/z.media_C4
            label='Factor C2/C4 [×]'
            filename=f'{top}_{metric}_C2_sobre_C4.png'
        else:
            y=z.media_C2-z.media_C4
            label='Incremento C2−C4'
            filename=f'{top}_{metric}_C2_menos_C4.png'
        fig,ax=makefig(f'{top}: {metrics[metric][0]}, {kind} C2/C4',label)
        if kind=='delta': ax.axhline(0,color='grey',lw=1)
        ax.plot(z.N,y,'-o')
        for x,v,a,b in zip(z.N,y,z.n_C2,z.n_C4):
            ax.annotate(f'n={int(a)}/{int(b)}',(x,v),xytext=(0,6),textcoords='offset points',ha='center',fontsize=8)
        save(fig,base/top/filename,'Solo puntos con C2 y C4 disponibles; n indica n_C2/n_C4.')

for metric,(title,ylabel) in metrics.items():
    z=d[(d.topology=='P2P')&(d.metric==metric)].copy()
    z['order']=z.condition.map({c:i for i,c in enumerate(conds)})
    z=z.sort_values('order')
    if z.empty: continue
    x=np.arange(len(z))
    fig,ax=makefig(f'P2P: {title} por condición',ylabel,'Condición')
    ax.bar(x,z.media,yerr=z['std'].fillna(0),capsize=4)
    ax.set_xticks(x,[names[c] for c in z.condition],rotation=15,ha='right')
    note_n(ax,x,z.media,z.n)
    save(fig,base/'P2P'/f'P2P_{metric}_por_condicion.png','Media ± desviación estándar. P2P tiene solo N=1.')

fig,ax=makefig('P2P: comparación de bytes de aplicación y radio','Bytes [bytes]','Condición')
ms=['app_bytes','radio_tx_bytes','radio_rx_bytes','radio_bytes']
cs=['C0','C2','C4']
x=np.arange(len(cs))
for j,m in enumerate(ms):
    vals=[]; errors=[]
    for c in cs:
        r=series('P2P',m,c)
        vals.append(float(r.media.iloc[0]) if len(r) else np.nan)
        errors.append(float(r['std'].fillna(0).iloc[0]) if len(r) else 0)
    ax.bar(x+(j-1.5)*.18,vals,width=.18,yerr=errors,capsize=3,label=metrics[m][0])
ax.set_xticks(x,[names[c] for c in cs],rotation=15,ha='right')
ax.legend(fontsize=8)
save(fig,base/'P2P'/'P2P_comparacion_app_tx_rx_total.png','Valores medios reconciliados, sin imputar nodos ausentes.')

allconds={
'radio_bytes':conds,'retransmission_pressure':conds,'mac_extra_attempts':conds,
'hqc_total_ticks_max':['C2'],'app_bytes':conds,
'radio_tx_bytes':['C2','C4'],'radio_rx_bytes':['C2','C4']}
for metric,cs in allconds.items():
    for c in cs:
        fig,ax=makefig(f'{metrics[metric][0]}: comparación entre topologías ({names[c]})',metrics[metric][1])
        for top in ['LINE','STAR','TREE']:
            z=series(top,metric,c)
            if z.empty: continue
            ax.errorbar(z.N,z.media,yerr=z['std'].fillna(0),fmt='-o',capsize=4,label=top)
            note_n(ax,z.N,z.media,z.n)
        ax.legend()
        save(fig,base/'COMPARATIVA_TOPOLOGIAS'/f'COMPARATIVA_{metric}_{c}.png','Media ± desviación estándar; n representa la cobertura de cada punto.')

for metric,kind in [('radio_bytes','ratio'),('app_bytes','delta'),('mac_extra_attempts','delta'),('retransmission_pressure','delta'),('nack_retransmitted_fragments','delta'),('retransmitted_bytes','delta')]:
    fig,ax=makefig(f'{metrics[metric][0]}: {kind} C2/C4 entre topologías','Factor C2/C4 [×]' if kind=='ratio' else 'Incremento C2−C4')
    for top in ['LINE','STAR','TREE']:
        z=pairs(top,metric)
        if kind=='ratio':z=z[z.media_C4!=0]
        if z.empty:continue
        y=z.media_C2/z.media_C4 if kind=='ratio' else z.media_C2-z.media_C4
        ax.plot(z.N,y,'-o',label=top)
        for x,v,a,b in zip(z.N,y,z.n_C2,z.n_C4):
            ax.annotate(f'n={int(a)}/{int(b)}',(x,v),xytext=(0,6),textcoords='offset points',ha='center',fontsize=8)
    ax.legend()
    save(fig,base/'COMPARATIVA_TOPOLOGIAS'/f'COMPARATIVA_{metric}_{kind}_C2_C4.png','Solo N con C2 y C4; ratio = media(C2)/media(C4).')

z=d[d.metric=='radio_bytes'].pivot_table(index=['topology','condition'],columns='N',values='n',aggfunc='first')
fig,ax=plt.subplots(figsize=(11,7),dpi=180)
im=ax.imshow(z.to_numpy(),aspect='auto')
ax.set(title='Cobertura experimental (n) para tráfico radio total',xlabel='Número de nodos (N)',ylabel='Topología y condición')
ax.set_xticks(range(len(z.columns)),list(z.columns))
ax.set_yticks(range(len(z.index)),[f'{a} - {b}' for a,b in z.index])
for i in range(len(z.index)):
    for j in range(len(z.columns)):
        v=z.iloc[i,j]
        if pd.notna(v): ax.text(j,i,int(v),ha='center',va='center',fontsize=8)
fig.colorbar(im,ax=ax,label='n ejecuciones')
fig.tight_layout()
fig.savefig(base/'COMPARATIVA_TOPOLOGIAS'/'COMPARATIVA_cobertura_n_radio_bytes.png',bbox_inches='tight')
plt.close(fig)
print('Imágenes creadas:',len(list(base.rglob('*.png'))))
