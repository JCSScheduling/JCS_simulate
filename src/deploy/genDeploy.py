import pandas as pd 
import numpy as np
import random, sys, argparse


def genTrace(hostnumber, iter, randomgen, hostperjob=3):
    hostlist = [i for i in range(0, hostnumber)]
    jobs = []
    # read model data
    models = pd.read_csv("../trace/ModelProfile.csv", usecols=["name", "parameter-MB", "memory-MB", "FLOPs-G"])
    # 均匀生成 每个job的worker数相同
    if(not randomgen):
        while(hostperjob <= len(hostlist)):
            modelindex = np.random.randint(0, models.shape[0]) #生成模型编号
            placement = random.sample(hostlist, hostperjob)
            jobinfo = {'jobid': len(jobs),
                    'ps_count': 1,
                    'worker_count': hostperjob-1,
                    'param_size': models.loc[modelindex, 'parameter-MB'],
                    'FLOPs': models.loc[modelindex, 'FLOPs-G'],
                    'mem_acc': models.loc[modelindex, 'memory-MB'],
                    'iter': iter,
                    'model':  models.loc[modelindex, 'name'],
                    'placement': placement}
            jobs.append(jobinfo)
            # delete host have been placed with job
            for item in placement:
                hostlist.remove(item)
        return jobs
    # 随机生成，每个job worker数不同    
    else:
        placefail = 0
        while(placefail < 20):
            # gpu数4-10
            # hostperjob = np.random.randint(4, 11)
            hostperjob = np.random.randint(6, 20)
            if(hostperjob <= len(hostlist)):
                modelindex = np.random.randint(0, models.shape[0]) #随机生成模型编号
                placement = random.sample(hostlist, hostperjob)
                jobinfo = {'jobid': len(jobs),
                        'ps_count': 1,
                        'worker_count': hostperjob-1,
                        'param_size': models.loc[modelindex, 'parameter-MB'],
                        'FLOPs': models.loc[modelindex, 'FLOPs-G'],
                        'mem_acc': models.loc[modelindex, 'memory-MB'],
                        'iter': iter,
                        'model':  models.loc[modelindex, 'name'],
                        'placement': placement}
                jobs.append(jobinfo)
                # delete host have been placed with job
                for item in placement:
                    hostlist.remove(item)
            else:
                placefail+=1
        return jobs

#Job-n-PS
#Job-n-Worker-m
def genActorName(filename, jobs):
    fout = open(filename + '.name', 'w')
    for job in jobs:
        psname = 'Job-'+ str(job['jobid']) + '-Node\n'
        fout.writelines(psname)
        for i in range(job['worker_count']):
            workername = 'Job-' + str(str(job['jobid'])) + '-PS-' + str(i) +'\n'
            fout.writelines(workername)
        for i in range(job['worker_count']):
            workername = 'Job-' + str(str(job['jobid'])) + '-Worker-' + str(i) +'\n'
            fout.writelines(workername)
    fout.writelines("Job-9999-Node")
    fout.close()





def toxml(filename, jobs):
    fxml = open(filename, 'w')
    # header
    header = """<?xml version='1.0'?>
    <!DOCTYPE platform SYSTEM "https://simgrid.org/simgrid.dtd">
    <platform version="4.1">\n"""
    fxml.writelines(header)

    scheinfo = r'<actor host="sche" function="Scheduler">' + '\n\t' + \
	    r'<argument value="' + str(len(jobs))+  r'"/> <!-- job number -->' + '\n' + \
        r'</actor>' + '\n'
    fxml.writelines(scheinfo)
    exitnode = """<actor host="sche" function="Job-9999-Node">
        <argument value="9999"/> <!-- jobid -->
        <argument value="2"/> <!-- worker id -->
        <argument value="5000"/> <!-- parameter size (MB) -->
        <argument value="4000"/> <!-- flops of this job (G flops) -->
        <argument value="4000"/> <!-- memory access of this job (in G), equivalent to flops -->
        <argument value="1"/> <!-- iter number -->
        <argument value="resnet-101"/> <!-- model name -->
    </actor>\n"""
    fxml.writelines(exitnode)

    for job in jobs:
        placement = job['placement']
        # generate JobNode deployment info
        psname = 'Job-'+ str(job['jobid']) + '-Node'
        fxml.writelines(r'<actor host="gpu' + str(placement[0]) + r'" function="'+ psname + '">\n')
        fxml.writelines('\t<argument ' + r'value="' + str(job['jobid']) + r'"/> <!-- jobid -->' + '\n')
        fxml.writelines('\t<argument ' + r'value="' + str(job['worker_count']) + r'"/> <!-- number of workers -->' + '\n')
        fxml.writelines('\t<argument ' + r'value="' + str(job['param_size']) + r'"/> <!-- parameter size (MB) -->' + '\n')
        fxml.writelines('\t<argument ' + r'value="' + str(job['FLOPs']) + r'"/> <!-- flops of this job (G flops) -->' + '\n')
        fxml.writelines('\t<argument ' + r'value="' + str(job['mem_acc']) + r'"/> <!-- memory access of this job (in G), equivalent to flops -->' + '\n')
        fxml.writelines('\t<argument ' + r'value="' + str(job['iter']) + r'"/> <!-- iter number -->' + '\n')
        fxml.writelines('\t<argument ' + r'value="' + job['model'] + r'"/>  <!-- model name -->' + '\n')
        fxml.writelines('</actor>' + '\n')
        # generate PS deployment info
        for workerid in range(job['worker_count']):
            workername = 'Job-' + str(str(job['jobid'])) + '-PS-' + str(workerid)
            fxml.writelines(r'<actor host="gpu' + str(placement[0]) + r'" function="' + workername + '">\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['jobid']) + r'"/> <!-- jobid -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(workerid) + r'"/> <!-- ps id -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['param_size']) + r'"/> <!-- parameter size (MB) -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['FLOPs']) + r'"/> <!-- flops of this job (G flops) -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['mem_acc']) + r'"/> <!-- memory access of this job (in G), equivalent to flops -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['iter']) + r'"/> <!-- iter number -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + job['model'] + r'"/> <!-- model name -->' + '\n')
            fxml.writelines('</actor>' + '\n')


        # generate Worker deployment info
        for workerid in range(job['worker_count']):
            workername = 'Job-' + str(str(job['jobid'])) + '-Worker-' + str(workerid)
            fxml.writelines(r'<actor host="gpu' + str(placement[workerid+1]) + r'" function="' + workername + '">\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['jobid']) + r'"/> <!-- jobid -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(workerid) + r'"/> <!-- worker id -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['param_size']) + r'"/> <!-- parameter size (MB) -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['FLOPs']) + r'"/> <!-- flops of this job (G flops) -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['mem_acc']) + r'"/> <!-- memory access of this job (in G), equivalent to flops -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + str(job['iter']) + r'"/> <!-- iter number -->' + '\n')
            fxml.writelines('\t<argument ' + r'value="' + job['model'] + r'"/> <!-- model name -->' + '\n')
            fxml.writelines('</actor>' + '\n')
    fxml.writelines('</platform>')

def def_parser():
    parser = argparse.ArgumentParser(description = "generate trace")
    parser.add_argument('-k', dest='k', help='k param of fatree topo', type=int, default=4, required=True)
    parser.add_argument('-t', '--iter', dest='t', help='iter number', type=int, default=100)
    parser.add_argument('-g', '--gpu number', dest='g', help='gpuperjob', type=int, default=0)
    return parser

def parse_args(parser):
    params = vars(parser.parse_args(sys.argv[1:]))
    return params

def main():
    args = parse_args(def_parser())
    gpunumber = args['k']**3*2
    iternum = args['t']
    gpuperjob = args['g']
    if(gpuperjob):
        randomgen = False
    else:
        randomgen = True
    jobs = genTrace(gpunumber, iternum, randomgen, gpuperjob)

    if randomgen:
        outfile = 'deploy-' + str(gpunumber//8) + '-' + str(len(jobs))  + '-random' + '.xml'
    else:
        outfile = 'deploy-' + str(gpunumber//8) + '-' + str(len(jobs))  + '-' + str(gpuperjob) + '.xml'
    toxml(outfile, jobs)
    genActorName(outfile, jobs)


if __name__ == "__main__":
    main()