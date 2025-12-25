#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <mqueue.h>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <set>

using namespace std;

const int MaxUsers = 5;
const int MaxUNameLen = 32;
const int PollInt = 2;      // seconds between file checks
const int BatchInt = 1;     // seconds between batch checks
const int OpThreshold = 5;  // global threshold to trigger merge
const string ShmName = "/synctext_registry";
const int ShmSize = 65536;
const string MasterFile = "centeralFile.txt";

enum OpType { INS, DEL, REP };

struct UpOp {
    OpType t;
    int ln, c0, c1;
    char oldC[256], newC[256], uid[MaxUNameLen];
    long long ts;   // timestamp in ms
    long long seq;  // per-process sequence number to improve uniqueness
    UpOp() : t(REP), ln(0), c0(0), c1(0), ts(0), seq(0) {
        memset(oldC, 0, sizeof(oldC));
        memset(newC, 0, sizeof(newC));
        memset(uid, 0, sizeof(uid));
    }
};

struct UserReg {
    char uid[MaxUNameLen], qn[64];
    atomic<bool> active;
    UserReg() : active(false) {
        memset(uid, 0, sizeof(uid));
        memset(qn, 0, sizeof(qn));
    }
};

struct ShReg {
    atomic<int> cnt;
    atomic<int> globalOpCount;
    UserReg u[MaxUsers];
    ShReg() : cnt(0), globalOpCount(0) {}
};

// Global state
atomic<bool> supMon(false), runn(true);
string uid, fileN, qn;
mqd_t mqid = (mqd_t)-1;
ShReg *regP = nullptr;
int shmfd = -1;

vector<UpOp> locOps, recOps;   // local and received buffers (thread-shared)
vector<string> prevC;
time_t lastMt = 0;

// seenOps used to dedupe already-applied operations
// NOTE: used across threads in original project; kept for compatibility.
// For strict lock-free safety this could be redesigned with a lock-free hashmap.
set<string> seenOps;

atomic<long long> localSeq(1); // per-process seq number for UpOp uniqueness

// utilities
void clr() { cout << "\033[2J\033[1;1H"; }

vector<string> rfile(const string &fn) {
    vector<string> v;
    ifstream f(fn);
    string l;
    while (getline(f, l)) v.push_back(l);
    return v;
}

void wfile(const string &fn, const vector<string> &v) {
    ofstream f(fn);
    for (auto &s : v) f << s << "\n";
}

time_t fmtime(const string &fn) {
    struct stat st;
    if (stat(fn.c_str(), &st) == 0) return st.st_mtime;
    return 0;
}

string opid(const UpOp &o) {
    stringstream ss;
    ss << o.uid << "|" << o.seq << "|" << o.ts << "|" << (int)o.t << "|" << o.ln << "|" << o.c0 << "|" << o.c1 << "|" << o.oldC << "|" << o.newC;
    return ss.str();
}

bool initshm() {
    shmfd = shm_open(ShmName.c_str(), O_CREAT | O_RDWR, 0666);
    if (shmfd == -1) {
        perror("shm_open");
        return false;
    }
    if (ftruncate(shmfd, ShmSize) == -1) {
        perror("ftruncate");
        close(shmfd);
        shmfd = -1;
        return false;
    }
    void *p = mmap(0, ShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        close(shmfd);
        shmfd = -1;
        return false;
    }
    regP = (ShReg *)p;
    bool any = false;
    for (int i = 0; i < MaxUsers; i++)
        if (regP->u[i].active.load()) { any = true; break; }
    if (!any) {
        memset(regP, 0, sizeof(ShReg));
        new (regP) ShReg();
    }
    return true;
}

bool regUser() {
    // Reconnect if already present
    for (int i = 0; i < MaxUsers; i++) {
        if (regP->u[i].active.load() && string(regP->u[i].uid) == uid) {
            strncpy(regP->u[i].qn, qn.c_str(), 63);
            cout << "Reconnected existing user: " << uid << "\n";
            return true;
        }
    }
    int n = regP->cnt.load();
    if (n >= MaxUsers) {
        cout << "Max users\n";
        return false;
    }
    for (int i = 0; i < MaxUsers; i++) {
        bool expected = false;
        if (regP->u[i].active.compare_exchange_strong(expected, true)) {
            strncpy(regP->u[i].uid, uid.c_str(), MaxUNameLen - 1);
            strncpy(regP->u[i].qn, qn.c_str(), 63);
            regP->cnt.fetch_add(1);
            cout << "New user registered: " << uid << "\n";
            return true;
        }
    }
    return false;
}

vector<string> activeUsers() {
    vector<string> v;
    for (int i = 0; i < MaxUsers; i++)
        if (regP->u[i].active.load()) v.push_back(string(regP->u[i].uid));
    return v;
}

bool initmq()
{
    struct mq_attr a{};
    a.mq_flags = 0;
    a.mq_maxmsg = 10;  // safe lower bound on most Linux systems
    a.mq_msgsize = sizeof(UpOp);
    a.mq_curmsgs = 0;

    // ensure queue name starts with '/' and is valid
    if (qn.empty() || qn[0] != '/')
        qn = "/" + qn;

    mq_unlink(qn.c_str());  // clean old queue (if any)

    mqid = mq_open(qn.c_str(), O_CREAT | O_RDWR | O_NONBLOCK, 0666, &a);
    if (mqid == (mqd_t)-1)
    {
        perror("mq_open");
        cerr << "Queue name: " << qn << "  msgsize=" << a.mq_msgsize
             << "  maxmsg=" << a.mq_maxmsg << "\n";
        return false;
    }
    return true;
}


void sendU(const string &q, const UpOp &o) {
    mqd_t m = mq_open(q.c_str(), O_WRONLY | O_NONBLOCK);
    if (m == (mqd_t)-1) {
        if (errno == ENOENT) {
            // try a few times to tolerate race where queue not ready yet
            for (int i = 0; i < 4 && m == (mqd_t)-1; i++) {
                this_thread::sleep_for(chrono::milliseconds(120));
                m = mq_open(q.c_str(), O_WRONLY | O_NONBLOCK);
            }
        }
    }
    if (m != (mqd_t)-1) {
        if (mq_send(m, (const char *)&o, sizeof(UpOp), 0) == -1) {
            // non-fatal, but log it
            cerr << "mq_send failed to " << q << " errno=" << errno << "\n";
        }
        mq_close(m);
    } else {
        // queue not present; log
        //cerr << "sendU: queue " << q << " not available\n";
    }
}

void bcast(const UpOp &o) {
    for (int i = 0; i < MaxUsers; i++) {
        if (regP->u[i].active.load()) {
            string u = regP->u[i].uid;
            if (u == uid) continue;
            sendU(regP->u[i].qn, o);
        }
    }
}

UpOp diffline(int ln, const string &a, const string &b) {
    UpOp o;
    o.ln = ln;
    strncpy(o.uid, uid.c_str(), MaxUNameLen - 1);
    o.ts = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
    o.seq = localSeq.fetch_add(1);
    int minl = min((int)a.size(), (int)b.size()), s = 0;
    while (s < minl && a[s] == b[s]) s++;
    int ae = (int)a.size() - 1, be = (int)b.size() - 1;
    while (ae >= s && be >= s && a[ae] == b[be]) { ae--; be--; }
    o.c0 = s;
    o.c1 = be + 1;
    string ao = (s <= ae) ? a.substr(s, ae - s + 1) : "";
    string bn = (s <= be) ? b.substr(s, be - s + 1) : "";
    strncpy(o.oldC, ao.c_str(), 255);
    strncpy(o.newC, bn.c_str(), 255);
    o.t = ao.empty() ? INS : bn.empty() ? DEL : REP;
    return o;
}

vector<UpOp> diff(const vector<string> &a, const vector<string> &b) {
    vector<UpOp> v;
    int n = max((int)a.size(), (int)b.size());
    for (int i = 0; i < n; i++) {
        string x = (i < a.size() ? a[i] : "");
        string y = (i < b.size() ? b[i] : "");
        if (x != y) v.push_back(diffline(i, x, y));
    }
    return v;
}

void show(const vector<string> &doc, const vector<UpOp> &changes) {
    clr();
    auto now = chrono::system_clock::to_time_t(chrono::system_clock::now());
    tm *lt = localtime(&now);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", lt);
    cout << "Document: " << fileN << "\n";
    cout << "Last updated: " << ts << "\n";
    cout << "----------------------------------------\n";
    for (size_t i = 0; i < doc.size(); i++) {
        bool mod = false;
        for (auto &x : changes) if ((size_t)x.ln == i) { mod = true; break; }
        cout << "Line " << i << ": " << doc[i];
        if (mod) cout << " [MODIFIED]";
        cout << "\n";
    }
    cout << "----------------------------------------\n";
    auto users = activeUsers();
    cout << "Active users: ";
    for (size_t i = 0; i < users.size(); i++) {
        cout << users[i];
        if (i + 1 < users.size()) cout << ", ";
    }
    cout << "\nMonitoring for changes...\n";
}

bool conf(const UpOp &a, const UpOp &b) {
    if (a.ln != b.ln) return false;
    return !(a.c1 <= b.c0 || b.c1 <= a.c0);
}

bool win(const UpOp &a, const UpOp &b) {
    if (a.ts != b.ts) return a.ts > b.ts;
    if (a.seq != b.seq) return a.seq > b.seq;
    return string(a.uid) < string(b.uid);
}

void apply(vector<string> &c, const UpOp &o) {
    int l = o.ln;
    while ((int)c.size() <= l) c.push_back("");
    string &ln = c[l];
    int s = o.c0, e = o.c1;
    switch (o.t) {
        case INS: {
            if (s > (int)ln.size()) s = ln.size();
            string n = o.newC;
            if (!(s + n.size() < ln.size() && ln.compare(s, n.size(), n) == 0))
                ln.insert(s, n);
            break;
        }
        case DEL: {
            if (s < (int)ln.size()) {
                string exp(o.oldC);
                if (s + (int)exp.size() <= (int)ln.size() && ln.substr(s, exp.size()) == exp)
                    ln.erase(s, exp.size());
            }
            break;
        }
        case REP: {
            string a = o.oldC, n = o.newC;
            if (s > (int)ln.size()) s = ln.size();
            if (e > (int)ln.size()) e = ln.size();
            if (a.size() > 0 && s + (int)a.size() <= (int)ln.size() && ln.substr(s, a.size()) == a)
                ln.replace(s, a.size(), n);
            else if (!(s + (int)n.size() < (int)ln.size() && ln.compare(s, n.size(), n) == 0))
                ln.insert(s, n);
            break;
        }
    }
}

vector<string> merge(vector<UpOp> &v) {
    vector<string> d = rfile(MasterFile);
    sort(v.begin(), v.end(), [](const UpOp &a, const UpOp &b) {
        if (a.ts != b.ts) return a.ts < b.ts;
        if (a.seq != b.seq) return a.seq < b.seq;
        return string(a.uid) < string(b.uid);
    });

    vector<UpOp> w;
    for (size_t i = 0; i < v.size(); i++) {
        bool ok = true;
        for (size_t j = 0; j < v.size(); j++) {
            if (i == j) continue;
            if (conf(v[i], v[j]) && !win(v[i], v[j])) {
                ok = false;
                break;
            }
        }
        if (ok) w.push_back(v[i]);
    }

    for (auto &o : w) {
        string id = opid(o);
        if (seenOps.count(id)) continue;
        apply(d, o);
        seenOps.insert(id);
    }
    return d;
}

// File monitor thread
void fmon() {
    while (runn) {
        time_t mt = fmtime(fileN);
        if (mt > lastMt) {
            if (supMon.load()) {
                prevC = rfile(fileN);
                lastMt = mt;
            } else {
                auto n = rfile(fileN);
                auto ch = diff(prevC, n);
                if (!ch.empty()) {
                    for (auto &o : ch) {
                        locOps.push_back(o);
                        regP->globalOpCount.fetch_add(1);

                        string oldc = string(o.oldC);
                        string newc = string(o.newC);
                        cout << "[local] Change detected: uid=" << uid
                             << " line=" << o.ln
                             << " cols=" << o.c0 << "-" << o.c1
                             << " \"" << oldc << "\" -> \"" << newc << "\"\n";
                    }
                    prevC = n;
                    lastMt = mt;
                    show(n, ch);
                } else {
                    prevC = n;
                    lastMt = mt;
                }
            }
        }
        this_thread::sleep_for(chrono::seconds(PollInt));
    }
}

// Listener thread - receives UpOp messages
void listen() {
    UpOp o;
    while (runn) {
        ssize_t b = mq_receive(mqid, (char *)&o, sizeof(UpOp), nullptr);
        if (b > 0) {
            string id = opid(o);
            // dedupe against seenOps to avoid multiple counting
            if (!seenOps.count(id)) {
                // dedupe within recOps to avoid double push
                bool found = false;
                for (auto &r : recOps) {
                    if (r.seq == o.seq && string(r.uid) == string(o.uid) && r.ts == o.ts) { found = true; break; }
                }
                if (!found) {
                    recOps.push_back(o);
                    regP->globalOpCount.fetch_add(1);
                    cout << "[recv] Received op from " << o.uid << " seq=" << o.seq << " ts=" << o.ts << "\n";
                }
            }
        } else {
            this_thread::sleep_for(chrono::milliseconds(50));
        }
    }
}

// keep queues warm / discover users
void refusers() {
    while (runn) {
        auto u = activeUsers();
        for (auto &x : u) {
            if (x == uid) continue;
            string q = "/queue_" + x;
            mqd_t t = mq_open(q.c_str(), O_WRONLY | O_NONBLOCK);
            if (t != (mqd_t)-1) mq_close(t);
        }
        this_thread::sleep_for(chrono::seconds(2));
    }
}

bool lead() {
    string b;
    bool f = false;
    for (int i = 0; i < MaxUsers; i++) {
        if (regP->u[i].active.load()) {
            string x = regP->u[i].uid;
            if (!f || x < b) { b = x; f = true; }
        }
    }
    return f && (b == uid);
}

// Batch thread - triggers merge when globalOpCount >= threshold
void batch() {
    static vector<UpOp> localBuffer;
    while (runn) {
        this_thread::sleep_for(chrono::milliseconds(50));

        // swap local and received ops into localsafe copies
        vector<UpOp> l, r;
        l.swap(locOps);
        r.swap(recOps);

        if (!l.empty()) {
            localBuffer.insert(localBuffer.end(), l.begin(), l.end());
        }

        int totalOps = regP->globalOpCount.load();
        if (totalOps < OpThreshold) continue;

        // 1) Broadcast our buffered local ops immediately so peers will receive them
        if (!localBuffer.empty()) {
            for (auto &o : localBuffer) bcast(o);
            cout << "[batch] Broadcasted " << localBuffer.size() << " local ops\n";
        }

        // 2)short pause to let messages propagate (tuned down)
        this_thread::sleep_for(chrono::milliseconds(30));   // just 30 ms


        // 3) collect late arrivals
        vector<UpOp> lateRec;
        lateRec.swap(recOps);
        vector<UpOp> lateLoc;
        lateLoc.swap(locOps);

        // aggregate everything: localBuffer + r (earlier rec) + lateRec + lateLoc
        vector<UpOp> allOps = localBuffer;
        allOps.insert(allOps.end(), r.begin(), r.end());
        allOps.insert(allOps.end(), lateRec.begin(), lateRec.end());
        allOps.insert(allOps.end(), lateLoc.begin(), lateLoc.end());

        // clear localBuffer now (we will re-populate if there are ops remaining later)
        localBuffer.clear();

        // remove duplicates in allOps via opid and seenOps check
        vector<UpOp> uniqueOps;
        set<string> localSeen;
        for (auto &o : allOps) {
            string id = opid(o);
            if (seenOps.count(id) || localSeen.count(id)) continue;
            uniqueOps.push_back(o);
            localSeen.insert(id);
        }

        if (uniqueOps.empty()) {
            // reset counter and continue
            regP->globalOpCount.store(0);
            continue;
        }

        cout << "[batch] Merging " << uniqueOps.size() << " ops (global count=" << totalOps << ")\n";

        // perform merge
        auto merged = merge(uniqueOps);

        // suspend monitoring while writing files to prevent self-detection
        supMon.store(true);
        if (lead()) {
            wfile(MasterFile, merged);
            cout << "[batch] Leader (" << uid << ") wrote master file (" << MasterFile << ")\n";
        }
        wfile(fileN, merged);
        prevC = merged;
        lastMt = fmtime(fileN);
        supMon.store(false);

        // show merged document (mark lines that were modified in this round)
        show(merged, uniqueOps);

        // broadcast all merged ops to ensure everyone converges
        for (auto &o : uniqueOps) bcast(o);

        // finally reset global counter
        regP->globalOpCount.store(0);
    }
}

void initdoc() {
    ifstream mchk(MasterFile);
    if (!mchk.good()) {
        ofstream m(MasterFile);
        m << "Hello World\n";
        m << "This is a collaborative editor\n";
        m << "Welcome to SyncText\n";
        m << "Edit this document and see real-time updates\n";
        m.close();
        cout << "Master file created with default content.\n";
    } else {
        mchk.seekg(0, ios::end);
        if (mchk.tellg() == 0) {
            mchk.close();
            ofstream m(MasterFile);
            m << "Hello World\n";
            m << "This is a collaborative editor\n";
            m << "Welcome to SyncText\n";
            m << "Edit this document and see real-time updates\n";
            m.close();
            cout << "Master file was empty; default content written.\n";
        } else {
            mchk.close();
        }
    }

    ifstream lchk(fileN);
    bool NewUserF = !lchk.good();
    lchk.close();
    time_t MastTime = fmtime(MasterFile), LocalTime = fmtime(fileN);
    if (NewUserF) {
        auto MastContent = rfile(MasterFile);
        wfile(fileN, MastContent);
        cout << "Created new file for user '" << uid << "' from master file.\n";
    } else if (LocalTime < MastTime) {
        auto MastContent = rfile(MasterFile);
        wfile(fileN, MastContent);
        cout << "Updated local file of '" << uid << "' from master (was outdated).\n";
    }
    prevC = rfile(fileN);
    lastMt = fmtime(fileN);
}

void clean() {
    for (int i = 0; i < MaxUsers; i++) {
        if (regP->u[i].active.load() && string(regP->u[i].uid) == uid) {
            regP->u[i].active.store(false);
            regP->cnt.fetch_sub(1);
            break;
        }
    }
    if (mqid != (mqd_t)-1) {
        mq_close(mqid);
        mq_unlink(qn.c_str());
    }
    if (regP) munmap(regP, ShmSize);
    if (shmfd != -1) close(shmfd);
}

int main(int c, char *v[]) {
    if (c != 2) {
        cout << "Usage: " << v[0] << " <user>\n";
        return 1;
    }

    uid = v[1];
    fileN = uid + "_doc.txt";
    qn = "/queue_" + uid;
    cout << "Start: " << uid << "\n";

    if (!initshm()) {
        cout << "shm fail\n";
        return 1;
    }
    if (!regUser()) {
        cout << "user fail\n";
        clean();
        return 1;
    }
    cout << "Registered\n";

    if (!initmq()) {
        cout << "mq fail\n";
        clean();
        return 1;
    }
    cout << "Queue ready\n";

    initdoc();
    vector<UpOp> e;
    show(prevC, e);

    // start threads
    thread t1(fmon), t2(listen), t3(refusers), t4(batch);

    // wait for threads (they run until program terminated)
    t1.join();
    t2.join();

    // signal stop and join remaining threads
    runn.store(false);
    t3.join();
    t4.join();

    clean();
    return 0;
}