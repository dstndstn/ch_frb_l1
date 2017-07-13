# Note: you probably want to run the webapp through the wrapper 
# script 'run-webapp.sh' in the toplevel ch_frb_l1 directory.
from __future__ import print_function


'''
FLASK_DEBUG=1 FLASK_APP=webapp.webapp WEBAPP_CONFIG=./l1_configs/l1_arun.yaml flask run --port 5002


FLASK_DEBUG=1 FLASK_APP=webapp.webapp WEBAPP_CONFIG=./l1_configs/l1_local.yml flask run --port 5002 &
./terminus-l1 /data1/chime/baseband_26m_processed/16k_B1937/0000004* &


'''

'''
prometheus -storage.local.path ~/chime/prometheus.db -config.file ~/chime/prometheus.yml
'''

'''
grafana-server --config=/usr/local/etc/grafana/grafana.ini --homepath /usr/local/share/grafana cfg:default.paths.logs=/usr/local/var/log/grafana cfg:default.paths.data=/usr/local/var/lib/grafana cfg:default.paths.plugins=/usr/local/var/lib/grafana/plugins &
'''

from flask import Flask, render_template, jsonify, request, make_response

import os
import re
import sys
import json
import yaml
import msgpack
import numpy as np

from rpc_client import RpcClient

#from flask import Flask, request, session, url_for, redirect, render_template, abort, g, flash, _app_ctx_stack

app = Flask(__name__)


def parse_config():
    """
    The webapp assumes that the WEBAPP_CONFIG environment variable is set to the
    name of a yaml config file.  

    Currently, this file only needs to contain the key 'rpc_address', which should 
    be a list of RPC server locations in the format 'tcp://10.0.0.101:5555'.  Note
    that an l1_config file will probably work, since it contains the 'rpc_address'
    key among others.

    This function parses the yaml file and returns the list of nodes, after some
    sanity checks.
    """

    if not os.environ.has_key('WEBAPP_CONFIG'):
        print("webapp: WEBAPP_CONFIG environment variable not set")
        print("  Maybe you want to run the webapp through the wrapper script")
        print("  'run-webapp.sh' in the toplevel ch_frb_l1 directory, which")
        print("  automatically sets this variable?")
        sys.exit(1)

    config_filename = os.environ['WEBAPP_CONFIG']

    if not os.path.exists(config_filename):
        print("webapp: config file '%s' not found" % config_filename)
        sys.exit(1)

    try:
        y = yaml.load(open(config_filename))
    except:
        print("webapp: couldn't parse yaml config file '%s'" % config_filename)
        sys.exit(1)

    if not isinstance(y,dict) or not y.has_key('rpc_address'):
        print("webapp: no 'rpc_address' field found in yaml file '%s'" % config_filename)
        sys.exit(1)

    nodes = y['rpc_address']

    if not isinstance(nodes,list) or not all(isinstance(x,basestring) for x in nodes):
        print("%s: expected 'rpc_address' field to be a list of strings" % config_filename)
        sys.exit(1)

    # FIXME(?): sanity-check the format of the node strings here?
    # (Should be something like 'tcp://10.0.0.101:5555'

    return nodes


app.nodes = parse_config()

client = None

def get_client():
    global client
    if client is None:
        client = RpcClient(dict([(''+str(i), k) for i,k in enumerate(app.nodes)]))
    return client

@app.route('/')
def index():
    return render_template('index.html', nodes=list(enumerate(app.nodes)),
                           nnodes = len(app.nodes),
                           node_status_url='/node-status')

@app.route('/2')
def index2():
    return render_template('index2.html', nodes=list(enumerate(app.nodes)),
                           node_status_url='/node-status')

@app.route('/node-status')
def node_status():
    #    a = request.args.get('a', 0, type=int)
    #j = jsonify([ dict(addr=k) for k in app.nodes ])

    client = get_client()
    # Make RPC requests for list_chunks and get_statistics asynchronously
    timeout = 5.

    ltokens = client.list_chunks(wait=False)
    stats = client.get_statistics(timeout=timeout)
    ch = client.wait_for_tokens(ltokens, timeout=timeout)
    ch = [msgpack.unpackb(p[0]) if p is not None else None
          for p in ch]
    
    # print('Chunks:')
    # for bch in ch:
    #     if bch is None:
    #         continue
    #     for b,f0,f1,w in bch:
    #         Nchunk = 1024 * 400
    #         print('  beam', b, 'chunk', f0/Nchunk, '+', (f1-f0)/Nchunk, 'from', w)

    #print('Stats[0]:', stats[0])

    stat = [ dict(addr=k, status='ok', chunks=chi, stats=st) for k,chi,st in zip(app.nodes, ch, stats) ]
    j = json.dumps(stat)
    # print('JSON:', j)
    return j

metric_counter = 1

@app.route('/metrics/')
def prometheus_metrics():
    global metric_counter
    rtn = ['# HELP chime_frb_l1_metrics_count Number of time Prometheus metrics have been polled',
           '# TYPE chime_frb_l1_metrics_count gauge',
           'chime_frb1_metrics_count %i' % metric_counter]
    metric_counter += 1

    client = get_client()
    # Make RPC requests for list_chunks asynchronously
    timeout = 5.
    # ltokens = client.list_chunks(wait=False)
    # ch = client.wait_for_tokens(ltokens, timeout=timeout)
    # ch = [msgpack.unpackb(p[0]) if p is not None else None
    #       for p in ch]
    # print('Chunks:', ch)

    name = 'chime_frb_l1_ringbuf_fpga_min'
    rtn.extend([
        '# HELP %s Ring buffer min FGPA counts' % name,
        '# TYPE %s gauge' % name,])
    name = 'chime_frb_l1_ringbuf_fpga_max'
    rtn.extend([
        '# HELP %s Ring buffer max FGPA counts' % name,
        '# TYPE %s gauge' % name,])
    name = 'chime_frb_l1_ringbuf_ntotal'
    rtn.extend(['# HELP %s Ring buffer current size' % name,
                '# TYPE %s gauge' % name,])

    stats = client.get_statistics(timeout=timeout)
    for i,(node,st) in enumerate(zip(app.nodes, stats)):
        #print('Node', i, ':', st)
        if st is None:
            continue
        node_stats, network_stats = st[:2]
        #print('Node', i, ':', st)
        # Ring buffer stats
        for ringbuf in st[2:]:
            beam = ringbuf['beam_id']
            name = 'chime_frb_l1_ringbuf_fpga_min{node="%03i", beam="%04i"}' % (i, beam)
            rtn.extend(['%s %i' % (name, ringbuf['ringbuf_fpga_min'])])
            name = 'chime_frb_l1_ringbuf_fpga_max{node="%03i", beam="%04i"}' % (i, beam)
            rtn.extend(['%s %i' % (name, ringbuf['ringbuf_fpga_max'])])
            name = 'chime_frb_l1_ringbuf_ntotal{node="%03i", beam="%04i"}' % (i, beam)
            rtn.extend(['%s %i' % (name, ringbuf['ringbuf_ntotal'])])

    #stat = [ dict(addr=k, status='ok', stats=st) for k,st in zip(app.nodes, stats) ]
    #print('Stats:', stat)
    
    txt = '\n'.join([str(x) for x in rtn] + [''])
    response = make_response(txt)
    response.headers['Content-type'] = 'text/plain'
    return response

@app.route('/node/<node_num>/')
def prometheus_metrics_node(node_num):
    if not re.match('^[0-9]{3}$', node_num):
        return 'Bad node number ' + node_num
    # Return metrics for a single node?
    name = 'chime_frb_l1_metrics_node_%s_randval' % node_num
    rtn = ['# HELP %s Some random value from node %s' % (name, node_num),
           '# TYPE %s gauge' % name,
           '%s %.3f' % (name, np.random.uniform(0, 10))]
    txt = '\n'.join(rtn + [''])
    response = make_response(txt)
    response.headers['Content-type'] = 'text/plain'
    return response



if __name__ == '__main__':
    app.run()
