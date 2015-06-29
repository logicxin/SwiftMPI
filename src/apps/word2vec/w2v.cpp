//#include <gflags/gflags.h>
#include <functional>
#include "../../swiftmpi.h"
using namespace swift_snails;

#define EXP_TABLE_SIZE 1000
#define MAX_EXP 6
const int table_size = 1e8;
/**
 * parameter vector's dimention
 */
int len_vec() {
    static int _len_vec = 0;
    if (_len_vec == 0) {
        _len_vec = global_config().get("word2vec", "len_vec").to_int32();
        CHECK_GT (_len_vec, 0);
    }
    return _len_vec;
}
/**
 * words will be std::hash-ed to size_t
 */
typedef size_t w2v_key_t;
/**
 * Word2Vec Server-side parameter type
 */
struct WParam {
    Vec h, v, h2sum, v2sum;

    WParam() {
        h.init(len_vec());    h.random();
        v.init(len_vec());    v.random();
        h2sum.init(len_vec());
        v2sum.init(len_vec());
    }
};
/**
 * Local parameter type
 */
struct WLocalParam {
    Vec h, v;

    WLocalParam() {
        h.init(len_vec());
        v.init(len_vec());
    }   
};
/**
 * Local gradient type
 */
struct WLocalGrad {
    Vec h_grad, v_grad;
    int h_count = 0, v_count = 0;

    WLocalGrad() {
        h_grad.init(len_vec());
        v_grad.init(len_vec());
        h_count = 0; v_count = 0;
    }

    void accu_h(const Vec& grad) {
        h_count ++;
        h_grad += grad;
    }

    void accu_v(const Vec& grad) {
        v_count ++;
        v_grad += grad;
    }

    void reset() {
        h_grad.clear();
        v_grad.clear();
        h_count = 0; v_count = 0;
    }
};

std::ostream& operator<< (std::ostream& os, const WParam &param) {
    os << param.v << "\t" << param.h;
    return os;
}
std::istream& operator>> (std::istream& is, WParam &param) {
    for (int i = 0; i < len_vec(); i++) {
        is >> param.v[i];
    }
    for (int i = 0; i < len_vec(); i++) {
        is >> param.h[i];
    }
    return is;
}
BinaryBuffer& operator<< (BinaryBuffer &bb, WLocalGrad &grad) {
    //CHECK_GT (grad.count, 0);
    if (grad.h_count > 0) grad.h_grad /= grad.h_count;
    if (grad.v_count > 0) grad.v_grad /= grad.v_count;
    for (int i = 0; i < len_vec(); i++) {
        bb << grad.h_grad[i];
        bb << grad.v_grad[i];
    }
    return bb;
}
BinaryBuffer& operator>> (BinaryBuffer &bb, WLocalGrad &grad) {
    for (int i = 0; i < len_vec(); i++) {
        bb >> grad.h_grad[i];
        bb >> grad.v_grad[i];
    }
    return bb;
}
BinaryBuffer& operator<< (BinaryBuffer &bb, WLocalParam &param) {
    for (int i = 0; i < len_vec(); i++) {
        bb << param.h[i]; 
        bb << param.v[i];
    }
    return bb;
}
BinaryBuffer& operator>> (BinaryBuffer &bb, WLocalParam &param) {
    for (int i = 0; i < len_vec(); i++) {
        bb >> param.h[i]; 
        bb >> param.v[i];
    }
    return bb;
}

class WPullAccessMethod : public PullAccessMethod<w2v_key_t, WParam, WLocalParam>
{
public:
    virtual void init_param(const w2v_key_t &key, param_t &param) {
    }
    virtual void get_pull_value(const w2v_key_t &key, const param_t &param, pull_t& val) noexcept
    {
        val.h = param.h;
        val.v = param.v;
    }
};

class WPushAccessMethod : public PushAccessMethod<w2v_key_t, WParam, WLocalGrad>
{
public:
    WPushAccessMethod() :
        initial_learning_rate( global_config().get("server", "initial_learning_rate").to_float())
    { }
    virtual void apply_push_value(const w2v_key_t &key, param_t &param, const grad_t& push_val) noexcept 
    {
        //LOG (INFO) << "apply push  " << key << "  param:" << param << "grad  " << push_val.h_grad << "  " << push_val.v_grad;
        param.h2sum += push_val.h_grad * push_val.h_grad;
        param.v2sum += push_val.v_grad * push_val.v_grad;
        param.h2sum += initial_learning_rate * push_val.h_grad / (swift_snails::sqrt(param.h2sum + fudge_factor));
        param.v2sum += initial_learning_rate * push_val.v_grad / (swift_snails::sqrt(param.v2sum + fudge_factor));
    }
private:
    float initial_learning_rate;
    static const float fudge_factor;
};
const float WPushAccessMethod::fudge_factor = 1e-6;

struct Instance {
    std::vector<w2v_key_t> words;

    void clear() {
        // clear data but not free memory
        words.clear();
    }
};  // end struct Instance

inline w2v_key_t hash_fn(const char* key) noexcept {
    return BKDRHash<w2v_key_t> (key);
}
/**
 * parse line to instance
 *
 * each word will be hashed to a size_t
 */
bool parse_instance(const std::string &line, Instance &ins) noexcept {
    static int min_length = 0;
    if (min_length == 0) 
        min_length = global_config().get("word2vec", "min_sentence_length").to_int32();

    auto words = std::move(swift_snails::split(line, " "));
    for (const auto& word : words) {
        ins.words.push_back(hash_fn(word.c_str()));
    }
    return ins.words.size() >=  min_length;
}

typedef ClusterServer<w2v_key_t, WParam, WLocalParam, WLocalGrad, WPullAccessMethod, WPushAccessMethod> server_t;
typedef GlobalPullAccess<w2v_key_t, WLocalParam, WLocalGrad> pull_access_t;
typedef GlobalPushAccess<w2v_key_t, WLocalParam, WLocalGrad> push_access_t;

std::shared_ptr<AsynExec::channel_t>& global_channel() {
    static AsynExec async(global_config().get("worker", "nthreads").to_int32());
    static auto channel = async.open();
    return channel;
}

class ExpTable {
public:
    typedef float real_t;

    ExpTable() {
        _table = new real_t[EXP_TABLE_SIZE + 1];
        //_table = (real_t *) malloc((EXP_TABLE_SIZE + 1) * sizeof(real_t));
        if (_table == nullptr) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        for (int i = 0; i < EXP_TABLE_SIZE; i++) {
            _table[i] = exp((i / (real_t) EXP_TABLE_SIZE * 2 - 1) * MAX_EXP); // Precompute the exp() table
            _table[i] = _table[i] / (_table[i] + 1); // Precompute f(x) = x / (x + 1)
	    }
    }

    real_t operator() (real_t f) noexcept {
        return _table[ (int) ((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
    }

    ~ExpTable() {
        if (_table != nullptr) {
            delete _table;
        }
    }

private:
    real_t *_table = nullptr;
};

ExpTable exptable;

/**
 * @brief negative-sampling will only sample within a minibatch
 *
 * Usage:
 *
 *  gather_keys()
 *  pull()
 *  ...
 *  push()
 */
class MiniBatch {
public:

    typedef LocalParamCache<w2v_key_t, WLocalParam, WLocalGrad> param_cache_t;
    /**
     * query parameters contained in local cache from remote server
     */
    MiniBatch ():
        _minibatch (global_config().get("worker", "minibatch").to_int32()),
        _nthreads (global_config().get("worker", "nthreads").to_int32()),
        _pull_access (global_pull_access<w2v_key_t, WLocalParam, WLocalGrad>()),
        _push_access (global_push_access<w2v_key_t, WLocalParam, WLocalGrad>())
    {
        CHECK_GT(_minibatch, 0);
        CHECK_GT(_nthreads, 0);
        AsynExec exec(_nthreads);
    }
    ~MiniBatch() {
        if(_table != nullptr) {
            delete _table;
            _table = nullptr;
        }
    }

    void pull() noexcept {
        LOG (INFO) << "... pull()";
        _pull_access.pull_with_barrier(_local_keys, _param_cache);
        LOG (INFO) << ">>> pull()";
        gen_unigram_table();
    }
    /**
     * update server-side parameters with local grad
     */
    void push() noexcept {
        _push_access.push_with_barrier(_local_keys, _param_cache);
        clear();
    }
    /**
     * gather keys within a minibatch
     *
     * @param minibatch size of the minibatch
     */
    size_t gather_keys (FILE* file, int minibatch = 0) noexcept {
        long cur_pos = ftell(file);
        std::atomic<int> line_count {0};
        LineFileReader line_reader;
        std::mutex file_mut;
        SpinLock spinlock1, spinlock2;
        _local_keys.clear();
        AsynExec::task_t handler = [this, &line_count, &line_reader,
            &file_mut, &spinlock1, &spinlock2, minibatch, &file
        ] {
            char *cline = nullptr;
            std::string line;
            Instance ins;
            std::map<w2v_key_t, int>::iterator word_freq_it;
            bool parse_res;
            while (true) {
                if (feof(file)) break;
                ins.clear();
                { std::lock_guard<std::mutex> lk(file_mut);
                    cline = line_reader.getline(file);
                    if (! cline) continue;
                    line = std::move(std::string(cline));
                }
                parse_res = parse_instance(line, ins);
                if (! parse_res) continue;
                for( const auto& item : ins.words) {
                    { std::lock_guard<SpinLock> lk(spinlock1);
                        _local_keys.insert(item);
                    }
                    { std::lock_guard<SpinLock> lk(spinlock2);
                        word_freq_it = _word_freq.find(item);
                        if (word_freq_it != _word_freq.end()) 
                            word_freq_it->second ++;
                        else 
                            _word_freq[item] = 1;
                    }
                }
                line_count ++;
                if(minibatch > 0 &&  line_count > minibatch) break;
                if (feof(file)) break;
            }
        };
        async_exec(_nthreads, handler, global_channel());
        RAW_LOG(INFO, "collect %d keys", _local_keys.size());
        fseek(file, cur_pos, SEEK_SET);
        return _local_keys.size();
    }

    param_cache_t& param() noexcept {
        return _param_cache;
    }

    w2v_key_t* table() noexcept {
        return _table;
    }

    void clear() noexcept {
        _local_keys.clear();
        _word_freq.clear();
        _wordids.clear();
    }
protected:
    /**
     * generate hashtable for Word2Vec's negative-samping
     */
    void gen_unigram_table() noexcept {
		LOG(INFO)<< "... init_unigram_table";
		CHECK_GT(_word_freq.size(), 0) << "word_freq should be inited before";
        // init wordids
        for (const auto& item : _word_freq) {
            _wordids.push_back(item.first);
        }
        CHECK(!_wordids.empty()) << "wordids should be inited before";
		int a, i;
		double train_words_pow = 0;
		double d1, power = 0.75;
        if(_table == nullptr) _table = new w2v_key_t[table_size];
        for (const auto& item : _word_freq) {
            train_words_pow += std::pow(item.second, power);
        }
        i = 0;
        d1 = pow(_word_freq[_wordids[i]], power) / (double)train_words_pow;
        for (a = 0; a < table_size; a++) {
            _table[a] = _wordids[i];
            if (a / (double)table_size > d1) {
                i ++;
                d1 += pow(_word_freq[_wordids[i]], power) / (double)train_words_pow;
            }
            if (i >= _word_freq.size()) i = _word_freq.size() - 1;
        }
    }
private:
    /**
     * @warning local_keys, word_freq, wordids should be consitant with each other
     */
    std::unordered_set<w2v_key_t> _local_keys;
    std::map<w2v_key_t, int> _word_freq;
    std::vector<w2v_key_t> _wordids;
    pull_access_t &_pull_access;
    push_access_t &_push_access;
    param_cache_t _param_cache;
    int _minibatch = 0; 
    int _nthreads = 0;  
    w2v_key_t *_table = nullptr;
};  // end class MiniBatch

struct Error {
    float data = 0;
    size_t counter = 0;

    void accu(float e) noexcept {
        data += e;
        counter ++;
    }
    
    float norm() noexcept {
        float error = data / counter;
        data = 0;
        counter = 0;
        return error;
    }
};

class Word2Vec {
public:
    Word2Vec (const std::string& path, int niters) :
        _batchsize (global_config().get("worker", "minibatch").to_int32()),
        _nthreads (global_config().get("worker", "nthreads").to_int32()),
        _window (global_config().get("word2vec", "window").to_int32()),
        _negative (global_config().get("word2vec", "negative").to_int32()),
        _alpha (global_config().get("word2vec", "learning_rate").to_float()),
        _niters (niters)
    {
        _path = path; 
        CHECK_GT(_path.size(), 0);
        CHECK_GT(_batchsize, 0);
        CHECK_GT(_nthreads, 0);
        CHECK_GT(_niters, 0);
    }

    void train() {
        LOG (WARNING) << "init train ...";
        LOG (INFO) << "first pull to init parameter";
        FILE* file = fopen(_path.c_str(), "rb");
        if (_minibatch.gather_keys(file) < 5) return;
        _minibatch.pull();
        global_mpi().barrier();
        _minibatch.clear();

        float error;
        for (int i = 0; i < _niters; i++) {
            LOG (INFO) << "iter\t" << i;
            error = train_iter(_path);
            LOG (INFO) << "iter\t" << i << "\terror:\t" << error;
        }
        fclose(file);
    }

    float train_iter(const std::string& path) noexcept {
        CHECK_GT(_batchsize, 0);
        FILE* file = fopen(_path.c_str(), "rb");
        std::mutex file_mut;
        std::atomic<int> line_count{0};
        LineFileReader line_reader(file);
        SpinLock spinlock;
        Instance ins;
        AsynExec::task_t handler = [this,
            &file, &file_mut, &line_reader,
            &line_count, &spinlock
            ] {
                std::string line;
                char* cline;
                Instance ins;
                bool parse_res;
                while (true) {
                    if (feof(file)) break;
                    { std::lock_guard<std::mutex> lk(file_mut);
                        cline = line_reader.getline();
                        if (! cline) continue;
                        line = std::move(std::string(cline));
                    }
                    parse_res = parse_instance(line, ins);
                    Vec neu1(len_vec()), neu1e(len_vec());
                    learn_instance(ins, neu1, neu1e);
                    line_count ++;
                    //LOG (INFO) << "line count:\t" << int(line_count);
                    if (line_count > _batchsize) break;
                    if (feof(file)) break;
                }
        };
        while (true) {
            line_count = 0;
            if (_minibatch.gather_keys(file, _batchsize) < 5) break;
            _minibatch.pull();
            async_exec(_nthreads, handler, global_channel());
            LOG (INFO) << "... push()";
            _minibatch.push();
        }
        fclose(file);
        return _error.norm();
    }

protected:
    void learn_instance (Instance &ins, Vec& neu1, Vec& neu1e) noexcept {
        neu1.clear(); neu1e.clear();
        int a, c, b = global_random()() % _window;
        int sent_length = ins.words.size();
        int pos = 0;
        int label;
        float g, f;
        w2v_key_t word, target, last_word;
        for (pos = 0; pos < sent_length; pos ++) {
            word = ins.words[pos];
            for (a = b; a < _window * 2 + 1 - b; a++) {
                if (a != _window) {
                    c = pos - _window + a;
                    if (c < 0 || c >= sent_length) continue;
                    last_word = ins.words[c];
                    Vec& syn0_lastword = _minibatch.param().params()[last_word].v;
                    neu1 += syn0_lastword;
                }
            }
            for (int d = 0; d < _negative + 1; d++) {
                if (d == 0) {
                    target = word;
                    label = 1;
                // generate negative samples
                } else {
                    target = _minibatch.table()[(global_random()() >> 16) % table_size];
                    if (target == 0) 
                        target = _minibatch.table()[(global_random()() >> 16) % table_size];
                    if (target == word) continue;
                    label = 0;
                }
                Vec& syn1neg_target = _minibatch.param().params()[target].h;
                f = 0;
                f += neu1.dot(syn1neg_target);
                if (f > MAX_EXP) g = (label - 1) * _alpha;
                else if (f < -MAX_EXP) g = (label - 0) * _alpha;
                else g = (label - exptable(f)) * _alpha;
                _error.accu(10000 * g * g);
                neu1e += g * syn1neg_target;
                _minibatch.param().grads()[target].accu_h(g * neu1);
            }
            // hidden -> in
            for (a = b; a < _window * 2 + 1 - b; a++) {
                if (a != _window) {
                    c = pos - _window + a;
                    if (c < 0 || c >= sent_length) continue;
                    last_word = ins.words[c];
                    Vec &syn0_lastword = _minibatch.param().params()[last_word].v;
                    _minibatch.param().grads()[last_word].accu_v( neu1e);
                }
            }
        }
    }
private:
    // dataset path
    std::string _path;
    int _batchsize; 
    int _nthreads;  
    int _niters;
    int _window;
    int _negative;
    std::unordered_set<w2v_key_t> _local_keys;
    float _alpha;   // learning rate
    MiniBatch _minibatch;
    Error _error;
};  // end class Word2Vec

using namespace std;
int main(int argc, char* argv[]) {
    GlobalMPI::initialize(argc, argv);
    // init config
    fms::CMDLine cmdline(argc, argv);
    std::string param_help         = cmdline.registerParameter("help",   "this screen");
    std::string param_config_path  = cmdline.registerParameter("config", "path of config file          \t[string]");
    std::string param_data_path    = cmdline.registerParameter("data",   "path of dataset, text only!  \t[string]");
    std::string param_niters       = cmdline.registerParameter("niters", "number of iterations         \t[int]");
    std::string param_param_output = cmdline.registerParameter("output", "path to output the parameters\t[string]");

    if(cmdline.hasParameter(param_help) || argc == 1) {
        cout << endl;
        cout << "===================================================================" << endl;
        cout << "   Word2Vec application" << endl;
        cout << "   Author: Suprjom <yanchunwei@outlook.com>" << endl;
        cout << "===================================================================" << endl;
        cmdline.print_help();
        cout << endl;
        cout << endl;
        return 0;
    }
    if (!cmdline.hasParameter(param_config_path) ||
        !cmdline.hasParameter(param_data_path) ||
        !cmdline.hasParameter(param_niters)
    ) {
        LOG(ERROR) << "missing parameter";
        cmdline.print_help();
        return 0;
    }
    std::string config_path = cmdline.getValue(param_config_path);
    std::string data_path   = cmdline.getValue(param_data_path);
    std::string output_path = cmdline.getValue(param_param_output);
    int niters              = stoi(cmdline.getValue(param_niters));
    global_config().load_conf(config_path);
    global_config().parse();

    // init cluster
    Cluster<ClusterWorker, server_t, w2v_key_t> cluster;
    cluster.initialize();

    Word2Vec w2v(data_path, niters);
    w2v.train();
    swift_snails::format_string(output_path, "-%d.txt", global_mpi().rank());
    RAW_LOG_WARNING ("server output parameter to %s", output_path.c_str());
    cluster.finalize(output_path);

    LOG(WARNING) << "cluster exit.";

    return 0;
}
