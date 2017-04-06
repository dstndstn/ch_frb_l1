#include <thread>
#include "l1-parts.hpp"
#include "chlog.hpp"

using namespace std;

// make_rfi_chain(): currently a placeholder which returns an arbitrarily constructed transform chain.
//
// The long-term plan here is:
//   - keep developing RFI removal, until all transforms are C++
//   - write code to serialize a C++ transform chain to yaml
//   - add a command-line argument <transform_chain.yaml> to ch-frb-l1 

// A little helper routine to make the bonsai_dedisperser 
// (Returns the rf_pipelines::wi_transform wrapper object, not the bonsai::dedisperser)
shared_ptr<rf_pipelines::wi_transform> make_dedisperser(const bonsai::config_params &cp, const shared_ptr<bonsai::trigger_output_stream> &tp)
{
    bonsai::dedisperser::initializer ini_params;
    ini_params.verbosity = 0;
    
    auto d = make_shared<bonsai::dedisperser> (cp, ini_params);
    d->add_processor(tp);

    return rf_pipelines::make_bonsai_dedisperser(d);
}

vector<shared_ptr<rf_pipelines::wi_transform>> make_rfi_chain()
{
    int nt_chunk = 1024;
    int polydeg = 2;

    auto t1 = rf_pipelines::make_polynomial_detrender(nt_chunk, rf_pipelines::AXIS_FREQ, polydeg);
    auto t2 = rf_pipelines::make_polynomial_detrender(nt_chunk, rf_pipelines::AXIS_TIME, polydeg);
    
    return { t1, t2 };
}

my_coarse_trigger_set::my_coarse_trigger_set() {}

/*
my_coarse_trigger_set::my_coarse_trigger_set(const bonsai::coarse_trigger_set& t)
 */

msgpack_config_serializer::msgpack_config_serializer() :
    bonsai::config_serializer("msgpack_config_serializer", false),
    sz(0)
{
}

msgpack_config_serializer::~msgpack_config_serializer() {}

void msgpack_config_serializer::write_param(const std::string &key, int val) {
    vals_i[key] = val;
}
void msgpack_config_serializer::write_param(const std::string &key, double val) {
    vals_d[key] = val;
}
void msgpack_config_serializer::write_param(const std::string &key, const std::string &val) {
    vals_s[key] = val;
}
void msgpack_config_serializer::write_param(const std::string &key, const std::vector<int> &val) {
    if (sz == 0)
        sz = val.size();
    else
        assert(val.size() == sz);
    vals_ivec[key] = val;
}
void msgpack_config_serializer::write_param(const std::string &key, const std::vector<double> &val) {
    if (sz == 0)
        sz = val.size();
    else
        assert(val.size() == sz);
    vals_dvec[key] = val;
}
void msgpack_config_serializer::write_param(const std::string &key, const std::vector<string> &val) {
    if (sz == 0)
        sz = val.size();
    else
        assert(val.size() == sz);
    vals_svec[key] = val;
}
void msgpack_config_serializer::write_analytic_variance(const float *in, const std::vector<int> &shape, int itree) {
}

int msgpack_config_serializer::size() {
    return sz;
}
void msgpack_config_serializer::pack(msgpack::sbuffer &buffer, int index) {
    assert(index >= 0);
    assert(index < sz);
    /*
     // copy the scalar values
     unordered_map<string, int> myvals_i(vals_i);
     // add the appropriate value pulled from the array values
     for (auto it=vals_ivec.begin(); it!=vals_ivec.end(); it++)
     myvals_i[it->first] = it->second[index];
     buffer.pack(myvals_i);
     // same for doubles
     unordered_map<string, double> myvals_d(vals_d);
     for (auto it=vals_dvec.begin(); it!=vals_dvec.end(); it++)
     myvals_d[it->first] = it->second[index];
     buffer.pack(myvals_d);
     // same for strings
     unordered_map<string, string> myvals_s(vals_s);
     // add the appropriate value pulled from the array values
     for (auto it=vals_svec.begin(); it!=vals_svec.end(); it++)
     myvals_s[it->first] = it->second[index];
     buffer.pack(myvals_s);
     */
    msgpack::packer<msgpack::sbuffer> pk(&buffer);
    pk.pack_map(vals_i.size() + vals_d.size() + vals_s.size() +
                vals_ivec.size() + vals_dvec.size() + vals_svec.size());
    for (auto it=vals_i.begin(); it!=vals_i.end(); it++) {
        pk.pack(it->first);
        pk.pack(it->second);
    }
    for (auto it=vals_ivec.begin(); it!=vals_ivec.end(); it++) {
        pk.pack(it->first);
        pk.pack(it->second[index]);
    }
    for (auto it=vals_d.begin(); it!=vals_d.end(); it++) {
        pk.pack(it->first);
        pk.pack(it->second);
    }
    for (auto it=vals_dvec.begin(); it!=vals_dvec.end(); it++) {
        pk.pack(it->first);
        pk.pack(it->second[index]);
    }
    for (auto it=vals_s.begin(); it!=vals_s.end(); it++) {
        pk.pack(it->first);
        pk.pack(it->second);
    }
    for (auto it=vals_svec.begin(); it!=vals_svec.end(); it++) {
        pk.pack(it->first);
        pk.pack(it->second[index]);
    }
}



l1b_trigger_stream::l1b_trigger_stream(zmq::context_t* ctx, string addr,
                                       bonsai::config_params bc) :
  // If "ctx" is not NULL, use that to create the socket, but don't keep track of it.
  // otherwise, create our own zmq context and use that to create the socket; save it
  // as zmqctx so we can delete it upon deletion of this object.
    zmqctx(ctx ? NULL : new zmq::context_t()),
    socket((ctx ? *ctx : *zmqctx), ZMQ_PUB),
    bonsai_config(bc),
    config_headers()
{
    cout << "Connecting socket to L1b at " << addr << endl;
    socket.connect(addr);

    bonsai_config.write(config_headers, true);
}

l1b_trigger_stream::~l1b_trigger_stream() {
    socket.close();
    if (zmqctx)
        delete zmqctx;
}

// helper for the next function
static void myfree(void* p, void*) {
    ::free(p);
}
// Convert a msgpack buffer to a ZeroMQ message; the buffer is released.
static zmq::message_t* sbuffer_to_message(msgpack::sbuffer &buffer) {
    zmq::message_t* msg = new zmq::message_t(buffer.data(), buffer.size(), myfree);
    buffer.release();
    return msg;
}

void l1b_trigger_stream::process_triggers(const std::vector<std::shared_ptr<bonsai::coarse_trigger_set> > &triggers, int ichunk) {
    msgpack::sbuffer buffer;
    assert(triggers.size() == bonsai_config.trigger_lag_dt.size());
    assert(triggers.size() == config_headers.size());

    msgpack::packer<msgpack::sbuffer> pk(&buffer);

    //vector<my_coarse_trigger_set> mytriggers;
    pk.pack_array(triggers.size() * 2);

    int i=0;
    for (auto it=triggers.begin(); it != triggers.end();
         it++, i++) {

        // ?
        config_headers.pack(buffer, i);

        my_coarse_trigger_set mt;
        mt.version = 1;
        mt.t0 = (*it)->t0;
        mt.fpgacounts0 = mt.t0 / rf_pipelines::constants::chime_seconds_per_fpga_count;
        mt.max_dm = bonsai_config.max_dm[i];
        mt.dt_sample = bonsai_config.dt_sample;
        mt.trigger_lag_dt = bonsai_config.trigger_lag_dt[i];
        mt.nt_chunk = bonsai_config.nt_chunk;
        mt.dm_coarse_graining_factor = (*it)->dm_coarse_graining_factor;
        mt.ndm_coarse = (*it)->ndm_coarse;
        mt.ndm_fine = (*it)->ndm_fine;
        mt.nt_coarse_per_chunk = (*it)->nt_coarse_per_chunk;
        mt.nsm = (*it)->nsm;
        mt.nbeta = (*it)->nbeta;
        mt.tm_stride_dm = (*it)->tm_stride_dm;
        mt.tm_stride_sm = (*it)->tm_stride_sm;
        mt.tm_stride_beta = (*it)->tm_stride_beta;
        mt.ntr_tot = mt.ndm_coarse * mt.nsm * mt.nbeta * mt.nt_coarse_per_chunk;
        mt.trigger_vec = vector<float>(mt.ntr_tot);

        float* triggers_data = &mt.trigger_vec[0];
        memcpy(triggers_data, (*it)->triggers, mt.ntr_tot * sizeof(float));
        //mytriggers.push_back(mt);

        pk.pack(mt);
    }
    //msgpack::pack(buffer, mytriggers);
    zmq::message_t* reply = sbuffer_to_message(buffer);
    chlog("Sending message of size: " << reply->size() << " to L1b");
    socket.send(*reply);
}

