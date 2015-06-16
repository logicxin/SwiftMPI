#include "../../swiftmpi.h"
using namespace swift_snals;
using namespace std;


typedef unsigned int key_t;

struct LRParam {
    float val = 0;
    float grad2sum = 0;
};

struct LRLocalParam {
    float val = 0;
};

struct LRLocalGrad {
    float val = 0;
    int count = 0;
};


class LRPullAccessMethod : public PullAccessMethod<key_t, LRParam, LRLocalParam, LRLocalGrad>
{
public:
    virtual void init_param(const key_t &key, param_t &param) {
        param.val = global_random().gen_float();  
    }
    virtual void get_pull_value(const key_t &key, const param_t &param, pull_t& val) {
        val.val = param.val;
    }
};


class LRPushAccessMethod : public PushAccessMethod<key_t, LRParam, LRLocalGrad>
{
public:
    /**
     * grad should be normalized before pushed
     */
    virtual void apply_push_value(const key_t& key, param_t &param, const push_val_t& push_val) {
        param.grad2sum += push_val.val * push_val.val;
        param.val += initial_learning_rate * push_val.val / sqrt(param.grad2sum + fudge_factor);
    }

private:
    float initial_learning_rate = global_config().get_config("server", "initial_learning_rate").to_float();
    static float fudge_factor;
};
float LRPullAccessMethod::fudge_factor = 1e-6;


LocalParamCache<key_t, LRLocalParam, LRLocalGrad> param_cache;

struct Instance {
    float target;
    std::vector< std::pair<unsigned int, float>> feas;

    void clear() {
        // clear data but not free memory
        feas.clear();
    }
};

void parse_instance(const char* line, Instance &ins) {
    char *cursor;
    unsigned int key;
    float value;
    CHECK((ins.target=strtod(line, &cursor), cursor != line)) << "target parse error!";
    line = cursor;
    while (*(line + count_spaces(line)) != 0) {
        CHECK((key = (unsigned int)strtoul(line, &cursor, 10), cursor != line));
        line = cursor;
        ins.feas.emplace_back(key, value);
    }
}


class LR {
public:
    typedef GlobalPullAccess<key_t, LRLocalParam, LRLocalGrad> pull_access_t;
    typedef GlobalPushAccess<key_t, LRLocalParam, LRLocalGrad> push_access_t;
    typedef LocalParamCache<key_t, LRLocalParam, LRLocalGrad> param_cache_t;

    LR (const string& path) : 
        _minibatch (global_config().get_config("worker", "minibatch").to_int32()),
        _nthreads (global_config().get_config("worker", "nthreads").to_int32()),
        _pull_access (global_pull_access<key_t, LRLocalParam, LRLocalGrad>()),
        _push_access (global_push_access<key_t, LRLocalParam, LRLocalGrad>())
    {
        _path = path; 
        AsynExec exec(_nthreads);
        _async_channel = exec.open();
    }

    void train() {
        init_keys();
        std::atomic<int> line_count {0};
        LineFileReader line_reader;
        std::mutex file_mut;
        SpinLock spinlock;

        FILE* file = fopen(_path, "rb");
        // first to init local keys
        gather_keys(file);
        pull();

        auto handler = [this, 
                &line_count, &line_reader, 
                &file, &file_mut, &spinlock]() {
            std::string line;
            float error;
            Instance ins;
            while (true) {
                { std::lock_guard<std::mutex> lk(file_mut);
                line = std::move(string(line_reader.getline(file))); 
                }
                parse_instance(line, ins);
                line_count ++;
                //if(ins.feas.size() < 4) continue;
                error = learn_instance(ins);
                line_count ++;
                if (line_count > _minibatch) break;
                if (feof(file)) break;
            }
        };
        while (true) {
            line_count = 0;
            gather_keys(file, _minibatch);
            pull();
            async_exec(_nthreads, handler, _async_channel);
            push();
            printf(
                "%cLines:%.2fk",
                13, float(line_count) / 1000);

            fflush(stdout);
            if (feof(file)) break;
        }
        LOG(WARNING) << "finish training ...";
    }
    /**
     * @brief gather keys within a minibatch
     * @param file file with fopen
     * @param minibatch size of Mini-batch, no limit if minibatch < 0
     */
    void gather_keys(FILE* file, int minibatch = -1) {
        long cur_pos = ftell(file);
        std::atomic<int> line_count {0};
        LineFileReader line_reader;
        std::mutex file_mut;
        SpinLock spinlock;
        _local_keys.clear();
        //CounterBarrier cbarrier(_nthreads);

        auto handler = [this, &line_count, &line_reader,
            &file_mut, &spinlock, minibatch
        ] {
            while (true) {
                { std::lock_guard<std::mutex> lk(file_mut);
                line = std::move(string(line_reader.getline(file))); 
                }
                parse_instance(line, ins);
                //if(ins.feas.size() < 4) continue;
                { std::lock_guard<SpinLock> lk(SpinLock);
                    for( const auto& item : ins.feas) {
                        _local_keys.insert(item.first);
                    }
                }
                line_count ++;
                if(minibatch > 0 &&  line_count > minibatch) break;
                if (feof(file)) break;
            }
        };
        async_exec(_nthreads, std::move(handler), _async_channel);
        RAW_DLOG(INFO, "collect %d keys", _local_keys.size());
        fseek(file, cur_pos, SEEK_SET);
    }
    /**
     * SGD update
     */
    float learn_instance(const Instance &ins) {
        float sum = 0;
        for (const auto& item : ins.feas) {
            auto param = _param_cache.params()[item.first].val;
            sum += param * item.second;
        }
        float predict = 1. / ( 1. + exp( - sum ));
        float error = ins.target - predict;
        // update grad 
        float grad = 0;
        for (const auto& item : ins.feas) {
            grad = error * item.second;
            _param_cache.grads()[item.first].val += grad;
            _param_cache.grads()[item.first].count ++;
        }
        return error * error;
    }

protected:
    /**
     * get local keys and init local parameter cache
     */
    void init_keys() {
        string line;
        LOG(WARNING) << "init local keys ...";
        ifstream file(_path);
        Instance ins;
        _local_keys.clear();
        while(getline(file, line)) {
            ins.clear();
            parse_instance(line.c_str(), ins);
            for (const auto& item : ins.feas) {
                _local_keys.insert(item.first);
            }
        }
        _param_cache.init_keys(_local_keys);
        file.close();
    }
    /**
     * query parameters contained in local cache from remote server
     */
    void pull() {
        _pull_access.pull_with_barrier(_local_keys, _param_cache);
    }
    /**
     * update server-side parameters with local grad
     */
    void push() {
        _push_access.push_with_barrier(_local_keys, _param_cache);
        _local_keys.clear();
    }

private:
    // dataset path
    string _path;
    int _minibatch; 
    int _nthreads;  
    int _niters;
    pull_access_t &_pull_access;
    push_access_t &_push_access;
    param_cache_t _param_cache;
    std::set<key_t> _local_keys;
    std::shared_ptr<AsynExec::channel_t> _async_channel;
};
