// Major features missing:
//
//   - RFI removal is a placeholder
//   - Alex Josephy's grouping/sifting code is not integrated
//   - Distributed logging is not integrated
//
// Currently hardcoded to assume the NUMA setup of the CHIMEFRB L1 nodes:
//   - Dual CPU
//   - 10 cores/cpu
//   - Hyperthreading enabled
//   - all NIC's on the same PCI-E bus as the first CPU.
//
// Note that the Linux scheduler defines 40 "cores":
//   cores 0-9:    primary hyperthread on CPU1 
//   cores 10-19:  primary hyperthread on CPU2
//   cores 20-29:  secondary hyperthread on CPU1
//   cores 30-39:  secondary hyperthread on CPU2

#include <thread>

#include <ch_frb_io.hpp>
#include <rf_pipelines.hpp>
#include <bonsai.hpp>
#include <l1-rpc.hpp>

#include "ch_frb_l1.hpp"
#include "chlog.hpp"
#include "l1-parts.hpp"

using namespace std;
using namespace ch_frb_l1;


// -------------------------------------------------------------------------------------------------
//
// More config parameters to come:
//  - Parameters defining location of sifting/grouping code (e.g. local socket)

struct l1_params {
    l1_params(const string &filename);

    // Input stream object reads UDP packets from correlator.
    shared_ptr<ch_frb_io::intensity_network_stream> make_input_stream(int istream);

    // L1-RPC object
    shared_ptr<L1RpcServer> make_l1rpc_server(int istream, shared_ptr<ch_frb_io::intensity_network_stream>);

    // nstreams is automatically determined by the number of (ipaddr, port) pairs.
    // There will be one (network_thread, assembler_thread) pair for each stream.
    int nbeams = 0;
    int nstreams = 0;

    // Both vectors have length nstream.
    vector<string> ipaddr;
    vector<int> port;

    // One L1-RPC per stream
    vector<string> rpc_address;

    // One L1b address per beam
    vector<string> l1b_address;
};


l1_params::l1_params(const string &filename)
{
    yaml_paramfile p(filename);
     
    this->nbeams = p.read_scalar<int> ("nbeams");
    this->ipaddr = p.read_vector<string> ("ipaddr");
    this->port = p.read_vector<int> ("port");
    this->rpc_address = p.read_vector<string> ("rpc_address");
    this->l1b_address = p.read_vector<string> ("l1b_address");
    
    if ((ipaddr.size() == 1) && (port.size() > 1))
	this->ipaddr = vector<string> (port.size(), ipaddr[0]);
    else if ((ipaddr.size() > 1) && (port.size() == 1))
	this->port = vector<int> (ipaddr.size(), port[0]);
    
    if (ipaddr.size() != port.size())
	throw runtime_error(filename + " expected 'ip_addr' and 'port' to be lists of equal length");
    
    this->nstreams = ipaddr.size();

    assert(nbeams > 0);
    assert(nstreams > 0);
    assert(ipaddr.size() == (unsigned int)nstreams);
    assert(port.size() == (unsigned int)nstreams);
    assert(rpc_address.size() == (unsigned int)nstreams);
    assert(l1b_address.size() == (unsigned int)nbeams);

    if (nbeams % nstreams) {
	throw runtime_error(filename + " nbeams (=" + to_string(nbeams) + ") must be a multiple of nstreams (="
			    + to_string(nstreams) + ", inferred from number of (ipaddr,port) pairs");
    }

    p.check_for_unused_params();
}


// l1_params::make_input_stream(): returns a stream object which will read packets from the correlator.

shared_ptr<ch_frb_io::intensity_network_stream> l1_params::make_input_stream(int istream)
{
    assert(istream >= 0 && istream < nstreams);

    if (std::thread::hardware_concurrency() != 40)
	throw runtime_error("ch-frb-l1: this program is currently hardcoded to run on the 20-core chimefrb test nodes, and won't run on smaller machines");

    if (nstreams % 2 == 1)
	throw runtime_error("ch-frb-l1: nstreams must be even, in order to divide dedispersion threads evenly between the two CPUs");

    int nbeams_per_stream = xdiv(nbeams, nstreams);
    
    ch_frb_io::intensity_network_stream::initializer ini_params;

    ini_params.ipaddr = ipaddr[istream];
    ini_params.udp_port = port[istream];
    ini_params.beam_ids = vrange(istream * nbeams_per_stream, (istream+1) * nbeams_per_stream);
    ini_params.mandate_fast_kernels = true;
    
    // Setting this flag means that an exception will be thrown if either:
    //
    //    1. the unassembled-packet ring buffer between the network and
    //       assembler threads is full (i.e. assembler thread is running slow)
    //
    //    2. the assembled_chunk ring buffer between the network and
    //       processing threads is full (i.e. processing thread is running slow)
    //
    // If we wanted, we could define separate flags for these two conditions.
    //
    // Note that in situation (2), the pipeline will crash anyway since
    // rf_pipelines doesn't contain code to handle gaps in the data.  This
    // is something that we'll fix soon, but it's nontrivial.
    
    ini_params.throw_exception_on_buffer_drop = true;

    // This disables the "telescoping" part of the telescoping ring buffers.
    // Currently, the telescoping logic is too slow for real-time use.  (The
    // symptom is that the assembler threads run slow, triggering condition (1)
    // from the previous comment.)  We should be able to fix this by writing
    // fancy assembly language kernels for the telescoping logic!

    ini_params.assembled_ringbuf_nlevels = 1;

    // Note that processing threads 0-7 are pinned to cores 0-7 (on CPU1)
    // and cores 10-17 (on CPU2).  I decided to pin assembler threads to
    // cores 8 and 18.  This leaves cores 9 and 19 free for RPC threads.

    if (istream < (nstreams/2))
	ini_params.assembler_thread_cores = {8,28};
    else
	ini_params.assembler_thread_cores = {18,38};

    // I decided to pin all network threads to CPU1, since according to
    // the motherboard manual, all NIC's live on the same PCI-E bus as CPU1.
    //
    // I think it makes sense to avoid pinning network threads to specific
    // cores on the CPU, since they use minimal cycles, but scheduling latency
    // is important for minimizing packet drops.  I haven't really tested this
    // assumption though!

    ini_params.network_thread_cores = vconcat(vrange(0,10), vrange(20,30));

    return ch_frb_io::intensity_network_stream::make(ini_params);
}


shared_ptr<L1RpcServer> l1_params::make_l1rpc_server(int istream, shared_ptr<ch_frb_io::intensity_network_stream> stream) {

    shared_ptr<L1RpcServer> rpc = make_shared<L1RpcServer>(stream, rpc_address[istream]);
    return rpc;
}


// -------------------------------------------------------------------------------------------------

static void dedispersion_thread_main(const l1_params &l1_config, const bonsai::config_params &cp,
				     const shared_ptr<ch_frb_io::intensity_network_stream> &sp, 
				     int ibeam)
{
    try {
	if (std::thread::hardware_concurrency() != 40)
	    throw runtime_error("ch-frb-l1: this program is currently hardcoded to run on the 20-core chimefrb test nodes, and won't run on smaller machines");
	
	if (l1_config.nbeams != 16)
	    throw runtime_error("ch-frb-l1: current core-pinning logic in dedispersion_thread_main() assumes 16 beams");
	
	// Pin thread before allocating anything (especially dedisperser!)
	int c = (ibeam / 8) * 10 + (ibeam % 8);
	ch_frb_io::pin_thread_to_cores({c,c+20});
	
        auto stream = rf_pipelines::make_chime_network_stream(sp, ibeam);
	auto transform_chain = make_rfi_chain();

	bonsai::dedisperser::initializer ini_params;
	ini_params.verbosity = 0;

	auto dedisperser = make_shared<bonsai::dedisperser> (cp, ini_params);
	
	// During development, it's convenient to throw in a bonsai::global_max_tracker,
	// so that the dedispersion thread can print the most significant (DM, arrival_time)
	// when it exits.
	//
	// FIXME: eventually the global_max_tracker can be removed (it won't be needed in production).
	
	auto max_tracker = make_shared<bonsai::global_max_tracker> ();
	dedisperser->add_processor(max_tracker);

	transform_chain.push_back(rf_pipelines::make_bonsai_dedisperser(dedisperser));

	// (transform_chain, outdir, json_output, verbosity)
	stream->run(transform_chain, string(), nullptr, 0);

	stringstream ss;
	ss << "bonsai: beam=" << ibeam 
	   << ", max_trigger=" << max_tracker->global_max_trigger
	   << ", dm=" << max_tracker->global_max_trigger_dm
	   << ", arrival_time=" << max_tracker->global_max_trigger_arrival_time << "\n";

	cout << ss.str().c_str() << flush;

    } catch (exception &e) {
	cerr << e.what() << "\n";
	throw;
    }
}


static void usage()
{
    cerr << "usage: ch-frb-l1 <l1_config.yaml> <bonsai_config.txt>\n";
    exit(2);
}


int main(int argc, char **argv)
{
    if (argc != 3)
	usage();

    l1_params l1_config(argv[1]);
    bonsai::config_params bonsai_config(argv[2]);

    int nstreams = l1_config.nstreams;
    int nbeams = l1_config.nbeams;

    vector<shared_ptr<ch_frb_io::intensity_network_stream>> input_streams(nstreams);
    vector<shared_ptr<L1RpcServer> > rpc_servers(nbeams);
    vector<std::thread> threads(nbeams);

    for (int istream = 0; istream < nstreams; istream++)
	input_streams[istream] = l1_config.make_input_stream(istream);

    for (int istream = 0; istream < nstreams; istream++) {
	rpc_servers[istream] = l1_config.make_l1rpc_server(istream, input_streams[istream]);
        // returns std::thread
        rpc_servers[istream]->start();
    }

    for (int ibeam = 0; ibeam < nbeams; ibeam++) {
	cerr << "spawning thread " << ibeam << endl;
	int nbeams_per_stream = xdiv(nbeams, nstreams);
	int istream = ibeam / nbeams_per_stream;
	threads[ibeam] = std::thread(dedispersion_thread_main, l1_config, bonsai_config, input_streams[istream], ibeam);
    }

    for (int ibeam = 0; ibeam < nbeams; ibeam++)
	threads[ibeam].join();

    for (int istream = 0; istream < nstreams; istream++) {
	cout << "stream " << istream << ": ipaddr=" << l1_config.ipaddr[istream] << ", udp_port=" << l1_config.port[istream] << endl;

	// vector<map<string,int>>
	auto statistics = input_streams[istream]->get_statistics();

	for (unsigned int irec = 0; irec < statistics.size(); irec++) {
	    cout << "    record " << irec  << endl;
	    const auto &s = statistics[irec];

	    vector<string> keys;
	    for (const auto &kv: s)
		keys.push_back(kv.first);
	    
	    sort(keys.begin(), keys.end());
	
	    for (const auto &k: keys) {
		auto kv = s.find(k);
		cout << "         " << k << " " << kv->second << endl;
	    }
	}
    }

    return 0;
}
