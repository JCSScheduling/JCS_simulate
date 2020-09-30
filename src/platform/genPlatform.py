import pandas as pd
import numpy as np
import random, argparse, sys
# host , hostspeed 
# router
# link bw, latency, sharing_policy
# route 
# link_ctn
# 拓扑：先用大交换机模型做实验，再用fattree
# GPU(Host): 先用典型GPU做同构，再做异构


# 6.20 TODO 集群拓扑架构按照PAI论文里的两种架构来生成
# 机内拓扑分为两种，机外拓扑统一fattree

def genHost(gpuname, gpunumber):
    gpu = pd.read_csv("../trace/GPUProfile.csv", usecols = ['index','name','bandwidth-GB-s','FLOPS-G'])
    hostlist = []
    # generate v100 homogeneous cluster
    if(gpuname == "v100" or gpuname == 'p100'):
        # print(gpu.loc[gpu['name'] == gpuname])
        speed = gpu.loc[gpu['name'] == gpuname].iloc[0]['FLOPS-G']
        membw = gpu.loc[gpu['name'] == gpuname].iloc[0]['bandwidth-GB-s']
        for i in range(0, gpunumber):
            gpuinfo = {'id': 'gpu'+str(i), 'speed': speed,
                    'membw':membw, 'gpuname': gpuname}
            hostlist.append(gpuinfo)
    else:
        # random generate
        hostgpu = np.random.randint(0, 13, size = gpunumber//8)
        gpuindex = []
        for typee in hostgpu:
            gpuindex.extend([typee for i in range(0, 8)])

        for i in range(0, gpunumber):
            gpuinfo = {'id': 'gpu' + str(i),
            'speed': gpu.loc[gpuindex[i], 'FLOPS-G'], 
            'membw': gpu.loc[gpuindex[i], 'bandwidth-GB-s'],
            'gpuname': gpu.loc[gpuindex[i], 'name']}
            hostlist.append(gpuinfo)

    # return format: [{'h0': 6000}, {'h1': 7500}, {'h2': 7500}]
    return hostlist

# host switch name: 'switch1'
# pcie switch name: 'swl1', 'swr1'

def genSwitch(gpunumber):
    switchs = []
    s = []
    sl = []
    sr = []
    if(gpunumber % 8 != 0):
        print("ERROR: gpunumber must be 8*n")
    for i in range(gpunumber//8):
        s.append({'id': 'sw'+str(i)})
        sl.append({'id': 'swl'+ str(i)})
        sr.append({'id': 'swr'+str(i)})
    switchs.append(s)
    switchs.append(sl)
    switchs.append(sr)
    return switchs

# ers: edge router(k/2), ars: aggregate router(k/2), crs: core router(k/2)^2
def genRouter(k):
    routerlist = []
    erlist = []
    arlist = []
    crlist = []
    for i in range((k**2)//2):
        erlist.append({'id': 'er' + str(i)})
        arlist.append({'id': 'ar' + str(i)})
    for i in range(((k//2)**2)):
        crlist.append({'id': 'cr' + str(i)})
    routerlist.append(erlist)
    routerlist.append(arlist)
    routerlist.append(crlist)
    return routerlist

# generate intra machine link, two tyoes: pcie, nvlink
def genIntraLink(gpus, switchs, mtype, bw,latency, links):
    linkinfo = {}
    # 生成switch到gpu的pcielink
    if(mtype == 'pcie' or mtype == 'nvlink'):
        # 每个machine3个switch
        s = switchs[0]
        sl = switchs[1]
        sr = switchs[2]
        for i in range(0, len(s)):
            gpuindex = i*8
            # 分别增加gpu与左/右侧seitch的link
            for gpu in range(gpuindex, gpuindex+8):
                if(gpu-gpuindex < 4):
                    switch = sl[i]
                else:
                    switch = sr[i]
                linkinfo = { 'id': 'l-'+ gpus[gpu]['id'] +'-'+ switch['id'],
                    'bandwidth':  str(bw['pcie']),
                        'latency':  str(latency['pcie']),
                'sharing_policy': "SPLITDUPLEX"}
                links.append(linkinfo)
            # 增加左侧switch和右侧switch链接到machine switch的link
            llinkinfo = { 'id': 'l-'+ sl[i]['id'] +'-'+ s[i]['id'],
                    'bandwidth':  str(bw['pcie']),
                        'latency':  str(latency['pcie']),
                'sharing_policy': "SPLITDUPLEX"}
            rlinkinfo = { 'id': 'l-'+ sr[i]['id'] +'-'+ s[i]['id'],
                    'bandwidth':  str(bw['pcie']),
                        'latency':  str(latency['pcie']),
                'sharing_policy': "SPLITDUPLEX"}
            links.append(llinkinfo)
            links.append(rlinkinfo)
    # 对nvlink结构，单独增加gpu间nvlink
    if(mtype == 'nvlink'):
        for i in range(0, len(gpus), 8):
            # 先生成左侧全连接
            for j in range(i, i+4):
                for k in range(j, i+4):
                    if(j == k):
                        continue
                    linkinfo = { 'id': 'l-'+ gpus[j]['id'] +'-'+ gpus[k]['id'],
                        'bandwidth':  str(bw['nvlink']),
                            'latency':  str(latency['nvlink']),
                    'sharing_policy': "SPLITDUPLEX"}
                    links.append(linkinfo)
            # 再生成右侧全连接
            for j in range(i+4, i+8):
                for k in range(j, i+8):
                    if(j == k):
                        continue
                    linkinfo = { 'id': 'l-'+ gpus[j]['id'] +'-'+ gpus[k]['id'],
                        'bandwidth':  str(bw['nvlink']),
                            'latency':  str(latency['nvlink']),
                    'sharing_policy': "SPLITDUPLEX"}
                    links.append(linkinfo)
            # 生成块间连接，gpu(i) to gpu(i+4)
            for j in range(i, i+4):
                linkinfo = { 'id': 'l-'+ gpus[j]['id'] +'-'+ gpus[j+4]['id'],
                    'bandwidth':  str(bw['nvlink']),
                        'latency':  str(latency['nvlink']),
                'sharing_policy': "SPLITDUPLEX"}
                links.append(linkinfo)


# gpus: hostlist, routers: routerlist, clutype: star,pcie,nvlink, 
# return format: 
def genClusterLink(gpus, switchs, routers, bw, latency, clutype, k, links):
    linkinfo = {}
    # bw = 2500.0  # 双向带宽2500，单向带宽1250； 10Gbps = 1.25GB/s = 1250MB/s, 40Gbps = 5000MBps
    if(clutype == 'star'):
        for i in range(0, len(gpus)):
            linkinfo = { 'id': 'l-'+str(gpus[i]['id'])+'-'+str(routers[0]['id']),
                'bandwidth':  str(bw['star']),
                'latency':  str(latency['ethernet']),
                'sharing_policy': "SPLITDUPLEX"}
            links.append(linkinfo)
    elif(clutype == 'fattree'):
        # 1. edgerouter to pcieswitch
        ers = routers[0]
        ars = routers[1]
        crs = routers[2]
        for i in range(0, len(ers)):
            gpuindex = i*(k//2)
            for j in range(gpuindex, gpuindex+(k//2)):
                linkinfo = { 'id': 'l-'+str(switchs[0][j]['id'])+'-'+str(ers[i]['id']),
                    'bandwidth':  str(bw['ethernet']),
                    'latency':  str(latency['ethernet']),
                    'sharing_policy': "SPLITDUPLEX"}
                links.append(linkinfo)
        # 2. aggrerouter to edge router
        for i in range(0, len(ers)):
            arindex = i - i%(k//2)
            for j in range(arindex, arindex + (k//2)):
                linkinfo = { 'id': 'l-'+str(ers[i]['id'])+'-'+str(ars[j]['id']),
                    'bandwidth':  str(bw['ethernet']),
                    'latency':  str(latency['ethernet']),
                    'sharing_policy': "SPLITDUPLEX"}
                links.append(linkinfo)
        # 3. core router to aggre router
        for arindex in range(0, len(ars), (k//2)):
            # pod中的每个ar都要连接k/2个cr
            crindex = 0
            for i in range(arindex, arindex + (k//2)):
                for j in range(crindex, crindex+(k//2)):
                    linkinfo = { 'id': 'l-'+str(ars[i]['id'])+'-'+str(crs[j]['id']),
                        'bandwidth':  str(bw['ethernet']),
                        'latency':  str(latency['ethernet']),
                        'sharing_policy': "SPLITDUPLEX"}
                    links.append(linkinfo)
                crindex += (k//2)    
    return links

def genScheduler(name):
    scheduler = {'id': name, 'speed': 1000}
    return scheduler

# 把scheduler连接到core router上即可
def genScheduleLink(scheduler, routers, links):
    crs = routers[2]
    linkinfo = { 'id': 'l-'+ crs[0]['id'] + '-' + scheduler['id'],
            'bandwidth':  '10000',
            'latency':  '0',
            'sharing_policy': "SPLITDUPLEX"}
    links.append(linkinfo)

def toxml(filename, scheduler, gpus, routers, switchs, links):
    fxml = open(filename, 'w')
    #1. head info
    header = """<?xml version='1.0'?>
    <!DOCTYPE platform SYSTEM "https://simgrid.org/simgrid.dtd">
    <platform version="4.1">
    <zone id="AS0" routing="Floyd">\n"""
    fxml.writelines(header)
    fxml.writelines('<!-- =====generate gpus===== -->\n')
    fxml.writelines('<host id="' + scheduler['id'] + '" speed="1000Gf"/>\n')
    #2. gpus, example: <host id="n0" speed="1Gf"/>
    for i in range(0, len(gpus)):
        hostinfo = r'<host id="' + gpus[i]['id'] + r'" speed="' + str(gpus[i]['speed']) + r'Gf">'+ \
        r'<prop id="membw" value="' + str(gpus[i]['membw']) + r'" />' + \
        r'<prop id="gpuname" value="' + gpus[i]['gpuname'] + r'" /></host>' +  '\n'
        fxml.writelines(hostinfo)

    #3. routers, example: <router id="r1"/>
    fxml.writelines('<!-- =====generate routers===== -->\n')
    for rindex in range(0, 3):
        for i in range(0, len(routers[rindex])):
            routerinfo = r'<router id="' + routers[rindex][i]['id'] + r'"/>' +'\n'
            fxml.writelines(routerinfo)
    for rindex in range(0, 3):    
        for i in range(0, len(switchs[rindex])):
            routerinfo = r'<router id="' + switchs[rindex][i]['id'] + r'"/>' +'\n'
            fxml.writelines(routerinfo)

    #4. links, example: <link id="l1" bandwidth="2.0GBps" latency="0ms" sharing_policy="SPLITDUPLEX"/>
    fxml.writelines('<!-- =====generate links===== -->\n')
    for i in range(0, len(links)):
        linksinfo = r'<link id="'+ links[i]['id'] +  \
                    r'" bandwidth="' + str(links[i]['bandwidth']) + r'MBps" ' + \
                    r'latency="' + str(links[i]['latency']) + r'ms" ' + \
                    r'sharing_policy="SPLITDUPLEX"/>'+'\n'
        fxml.writelines(linksinfo)
    
    #5.route, example:     
    # <route src="h0" dst="r0" symmetrical='no'>
    # 	<link_ctn id="l-h0-r0" direction="UP"/>
    # </route>
    # <route src="r0" dst="h0" symmetrical='no'>
    # 	<link_ctn id="l-h0-r0" direction="DOWN"/>
    # </route>
    fxml.writelines('<!-- =====generate route===== -->\n')
    for i in range(0, len(links)):
        src = links[i]['id'].split('-')[1]
        dst = links[i]['id'].split('-')[2]
        fxml.writelines(r'<route src="' + src + r'" dst="' + dst + r'" symmetrical="no">'+ '\n')
        fxml.writelines('\t<link_ctn '+r'id="'+links[i]['id'] + r'" direction="UP"/>' +'\n')
        fxml.writelines('</route>\n')
        fxml.writelines(r'<route src="' + dst + r'" dst="' + src + r'" symmetrical="no">'+ '\n')
        fxml.writelines('\t<link_ctn '+r'id="'+links[i]['id'] + r'" direction="DOWN"/>' +'\n')
        fxml.writelines('</route>\n')


    #6. tail info
    tail = "\t</zone>\n</platform>"
    fxml.writelines(tail)

def def_parser():
    parser = argparse.ArgumentParser(description = "generate platform")
    parser.add_argument('-k', dest='k', help='k param of fatree topo', type=int, default=4, required=True)
    parser.add_argument('-m', '--machine', dest='m', help='machinetype, pcie/nvlink', type=str, default='pcie')
    parser.add_argument('-c', '--clutype', dest='c', help='clutype, star/fattree', type=str, default='fattree')
    parser.add_argument('-g', '--gpu', dest='g', help='gputype, mix/v100/p100', type=str, default='mix')
    return parser

def parse_args(parser):
    params = vars(parser.parse_args(sys.argv[1:]))
    return params


def main():
    args = parse_args(def_parser())
    k = args['k']  # fattree k, hostnummber = (k**3)/4
    machinetype = args['m'] # pcie/nvlink
    clutype = args['c']  # star/fattree
    gputype = args['g']  #mix, v100, p100 or something else
    gpunumber = int(k**3/4*8)
    # 双向带宽10000，单向带宽5000； 10Gbps = 1.25GB/s = 1250MB/s, 1Gbps = 125MB/s, 25Gbps=3125MB/s, 40Gbps=5000MB/s, 100Gbps=12500MB/s
    # ethernet: 40Gbps = 5000MBps
    # pcie: 80Gbps = 10000MBps = 10GBps
    # nvlink: 200Gbps = 25000MBps = 25GBps
    bw = {'ethernet': 12500, 'pcie': 10000, 'nvlink': 25000}
    latency = {'ethernet': 0, 'pcie': 0, 'nvlink': 0}
    links = []

    outfile =  machinetype + '-' + str(gpunumber//8) + '-' + gputype + '-100Gbps' +'.xml'




    gpus = genHost(gputype, gpunumber)
    switchs = genSwitch(gpunumber)
    routers = genRouter(k)
    scheduler = genScheduler('sche')
    genIntraLink(gpus, switchs, machinetype, bw, latency, links)
    genClusterLink(gpus, switchs, routers, bw, latency, clutype, k ,links)
    genScheduleLink(scheduler, routers, links)
    toxml(outfile, scheduler, gpus, routers, switchs, links)


if __name__ == '__main__':
    main()



