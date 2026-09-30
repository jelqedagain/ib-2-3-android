// NSCondition, NSConditionLock, NSOperation/NSOperationQueue, NSCache, NSURLConnection & friends
// (always offline), NSKeyedArchiver, NSJSONSerialization, formatters.
#include "foundation/foundation.h"
#include "foundation/runloop.h"
#include "libc/pthread.h"
#include "objc/internal.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <windows.h>

namespace ns {

namespace {

struct CondData : objc::HostData {
    std::recursive_mutex m;
    std::condition_variable_any cv;
    s64 condition = 0;
};
CondData& cond(id o) { return objc::ensure<CondData>(o); }

struct OpData : objc::HostData {
    GuestAddr block = 0;
    id target = 0;
    SEL sel = 0;
    id arg = 0;
    bool finished = false, cancelled = false;
};

id offline_error() {
    return objc::send(objc::class_named("NSError"), "errorWithDomain:code:userInfo:",
                      {str("NSURLErrorDomain"), (u64)(s64)-1009, dict({{str("NSLocalizedDescription"), str("The Internet connection appears to be offline.")}})});
}

struct RequestData : objc::HostData {
    id url = 0;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<u8> body;
};

struct ResponseData : objc::HostData {
    id url = 0;
    int status = 200;
    std::string content_type;
    u64 length = 0;
};

std::function<bool(const HttpRequest&, HttpResponse&)> g_local_server;

void set_header(RequestData& d, const std::string& name, const std::string& value) {
    for (auto& [k, v] : d.headers)
        if (_stricmp(k.c_str(), name.c_str()) == 0) {
            v = value;
            return;
        }
    d.headers.push_back({name, value});
}

// The response for a request the port serves itself, or false (the request fails as offline).
bool serve(id req, HttpResponse& out) {
    if (!g_local_server || !req) return false;
    auto* d = objc::get<RequestData>(req);
    if (!d) return false;
    HttpRequest r{d->method, utf8(objc::send(d->url, "absoluteString")), d->headers, d->body};
    if (!g_local_server(r, out)) {
        LOG_DEBUG("http: %s %s -> offline", r.method.c_str(), r.url.c_str());
        return false;
    }
    LOG_INFO("http: %s %s -> %d (%zu bytes, served by the port)", r.method.c_str(), r.url.c_str(), out.status, out.body.size());
    return true;
}

id make_response(id req, const HttpResponse& r) {
    id resp = objc::alloc(objc::class_named("NSHTTPURLResponse"));
    auto& d = objc::ensure<ResponseData>(resp);
    d.url = objc::retain(objc::send(req, "URL"));
    d.status = r.status;
    d.content_type = r.content_type;
    d.length = r.body.size();
    return objc::autorelease(resp);
}

}  // namespace

void set_local_server(std::function<bool(const HttpRequest&, HttpResponse&)> server) { g_local_server = std::move(server); }

void install_misc() {
    using objc::class_method;
    using objc::method;

    // ---- NSCondition ----
    Class C = objc::host_class("NSCondition");
    method(C, "init", [](id self, SEL) {
        cond(self);
        return self;
    });
    method(C, "lock", [](id self, SEL) { cond(self).m.lock(); });
    method(C, "unlock", [](id self, SEL) { cond(self).m.unlock(); });
    method(C, "wait", [](id self, SEL) {
        auto& c = cond(self);
        c.cv.wait(c.m);
    });
    method(C, "waitUntilDate:", [](id self, SEL, id date) {
        auto& c = cond(self);
        double w = date_seconds(date) - now_ref();
        if (w <= 0) return false;
        return c.cv.wait_for(c.m, std::chrono::microseconds((s64)(w * 1e6))) == std::cv_status::no_timeout;
    });
    method(C, "signal", [](id self, SEL) { cond(self).cv.notify_one(); });
    method(C, "broadcast", [](id self, SEL) { cond(self).cv.notify_all(); });
    method(C, "setName:", [](id, SEL, id) {});

    // ---- NSConditionLock ----
    Class CL = objc::host_class("NSConditionLock");
    method(CL, "init", [](id self, SEL) {
        cond(self);
        return self;
    });
    method(CL, "initWithCondition:", [](id self, SEL, s64 c) {
        cond(self).condition = c;
        return self;
    });
    method(CL, "condition", [](id self, SEL) -> s64 { return cond(self).condition; });
    method(CL, "lock", [](id self, SEL) { cond(self).m.lock(); });
    method(CL, "unlock", [](id self, SEL) {
        cond(self).m.unlock();
        cond(self).cv.notify_all();
    });
    method(CL, "tryLock", [](id self, SEL) { return cond(self).m.try_lock(); });
    method(CL, "lockWhenCondition:", [](id self, SEL, s64 v) {
        auto& c = cond(self);
        c.m.lock();
        while (c.condition != v) c.cv.wait(c.m);
    });
    method(CL, "tryLockWhenCondition:", [](id self, SEL, s64 v) {
        auto& c = cond(self);
        if (!c.m.try_lock()) return false;
        if (c.condition != v) {
            c.m.unlock();
            return false;
        }
        return true;
    });
    method(CL, "unlockWithCondition:", [](id self, SEL, s64 v) {
        auto& c = cond(self);
        c.condition = v;
        c.m.unlock();
        c.cv.notify_all();
    });

    // ---- NSOperation / NSOperationQueue ----
    Class OP = objc::host_class("NSOperation");
    Class BOP = objc::host_class("NSBlockOperation", "NSOperation");
    Class IOP = objc::host_class("NSInvocationOperation", "NSOperation");
    Class OQ = objc::host_class("NSOperationQueue");
    method(OP, "start", [](id self, SEL) {
        auto& d = objc::ensure<OpData>(self);
        if (!d.cancelled) objc::send(self, "main");
        d.finished = true;
    });
    method(OP, "main", [](id self, SEL) {
        auto& d = objc::ensure<OpData>(self);
        if (d.block) objc::call_block(d.block);
        else if (d.target) objc::send_sel(d.target, d.sel, {d.arg});
    });
    method(OP, "cancel", [](id self, SEL) { objc::ensure<OpData>(self).cancelled = true; });
    method(OP, "isCancelled", [](id self, SEL) { return objc::ensure<OpData>(self).cancelled; });
    method(OP, "isFinished", [](id self, SEL) { return objc::ensure<OpData>(self).finished; });
    method(OP, "isExecuting", [](id, SEL) { return false; });
    method(OP, "isConcurrent", [](id, SEL) { return false; });
    method(OP, "setQueuePriority:", [](id, SEL, s64) {});
    method(OP, "setCompletionBlock:", [](id, SEL, GuestAddr) {});
    method(OP, "waitUntilFinished", [](id self, SEL) {
        while (!objc::ensure<OpData>(self).finished) Sleep(1);
    });
    class_method(BOP, "blockOperationWithBlock:", [](Class c, SEL, GuestAddr b) {
        id o = objc::alloc(c);
        objc::ensure<OpData>(o).block = objc::block_copy(b);
        return objc::autorelease(o);
    });
    method(BOP, "addExecutionBlock:", [](id self, SEL, GuestAddr b) { objc::ensure<OpData>(self).block = objc::block_copy(b); });
    method(IOP, "initWithTarget:selector:object:", [](id self, SEL, id target, SEL s, id arg) {
        auto& d = objc::ensure<OpData>(self);
        d.target = objc::retain(target);
        d.sel = s;
        d.arg = objc::retain(arg);
        return self;
    });
    static id s_main_queue = 0;
    class_method(OQ, "mainQueue", [](Class c, SEL) {
        if (!s_main_queue) s_main_queue = objc::alloc(c);
        return s_main_queue;
    });
    class_method(OQ, "currentQueue", [](Class c, SEL) -> id { return is_main_thread() ? objc::send(c, "mainQueue") : 0; });
    auto run_op = [](id queue, id op) {
        objc::retain(op);
        auto body = [op] {
            objc::send(op, "start");
            objc::release(op);
        };
        if (queue == s_main_queue) post_to_main(body);
        else libc::spawn_guest_thread("NSOperation", body);
    };
    static decltype(run_op) s_run_op = run_op;
    method(OQ, "addOperation:", [](id self, SEL, id op) { s_run_op(self, op); });
    method(OQ, "addOperationWithBlock:", [](id self, SEL, GuestAddr b) {
        s_run_op(self, objc::send(objc::class_named("NSBlockOperation"), "blockOperationWithBlock:", {b}));
    });
    method(OQ, "addOperations:waitUntilFinished:", [](id self, SEL, id ops, bool) {
        for (id op : array_items(ops)) s_run_op(self, op);
    });
    for (const char* s : {"cancelAllOperations", "waitUntilAllOperationsAreFinished"}) method(OQ, s, [](id, SEL) {});
    method(OQ, "setMaxConcurrentOperationCount:", [](id, SEL, s64) {});
    method(OQ, "setName:", [](id, SEL, id) {});
    method(OQ, "setSuspended:", [](id, SEL, bool) {});
    method(OQ, "operationCount", [](id, SEL) -> u64 { return 0; });
    method(OQ, "operations", [](id, SEL) { return array({}); });

    // ---- NSCache (backed by a dictionary) ----
    Class CA = objc::host_class("NSCache");
    struct CacheData : objc::HostData {
        id dict = 0;
    };
    auto cache_dict = [](id self) -> id {
        auto& d = objc::ensure<CacheData>(self);
        if (!d.dict) d.dict = objc::retain(mutable_dict());
        return d.dict;
    };
    static decltype(cache_dict) s_cache = cache_dict;
    method(CA, "objectForKey:", [](id self, SEL, id k) { return dict_get(s_cache(self), k); });
    method(CA, "setObject:forKey:", [](id self, SEL, id v, id k) { dict_set(s_cache(self), k, v); });
    method(CA, "setObject:forKey:cost:", [](id self, SEL, id v, id k, u64) { dict_set(s_cache(self), k, v); });
    method(CA, "removeObjectForKey:", [](id self, SEL, id k) { objc::send(s_cache(self), "removeObjectForKey:", {k}); });
    method(CA, "removeAllObjects", [](id self, SEL) { objc::send(s_cache(self), "removeAllObjects"); });
    for (const char* s : {"setCountLimit:", "setTotalCostLimit:"}) method(CA, s, [](id, SEL, u64) {});
    method(CA, "setName:", [](id, SEL, id) {});
    method(CA, "setDelegate:", [](id, SEL, id) {});

    // ---- networking: offline, except requests the port serves itself (set_local_server) ----
    Class RQ = objc::host_class("NSURLRequest");
    Class MRQ = objc::host_class("NSMutableURLRequest", "NSURLRequest");
    auto make_request = [](Class c, id url) {
        id r = objc::alloc(c);
        objc::ensure<RequestData>(r).url = objc::retain(url);
        return objc::autorelease(r);
    };
    static decltype(make_request) s_make_request = make_request;
    class_method(RQ, "requestWithURL:", [](Class c, SEL, id url) { return s_make_request(c, url); });
    objc::add_method(RQ, "requestWithURL:cachePolicy:timeoutInterval:", [](cpu::Thread& t) { t.set_x(0, s_make_request(t.x(0), t.x(2))); }, true);
    method(RQ, "initWithURL:", [](id self, SEL, id url) {
        objc::ensure<RequestData>(self).url = objc::retain(url);
        return self;
    });
    objc::add_method(RQ, "initWithURL:cachePolicy:timeoutInterval:", [](cpu::Thread& t) {
        objc::ensure<RequestData>(t.x(0)).url = objc::retain(t.x(2));
    });
    method(RQ, "URL", [](id self, SEL) { return objc::ensure<RequestData>(self).url; });
    method(RQ, "HTTPMethod", [](id self, SEL) { return str(objc::ensure<RequestData>(self).method); });
    method(RQ, "allHTTPHeaderFields", [](id self, SEL) {
        std::vector<std::pair<id, id>> items;
        for (auto& [k, v] : objc::ensure<RequestData>(self).headers) items.push_back({str(k), str(v)});
        return dict(items);
    });
    method(RQ, "valueForHTTPHeaderField:", [](id self, SEL, id name) -> id {
        std::string n = utf8(name);
        for (auto& [k, v] : objc::ensure<RequestData>(self).headers)
            if (_stricmp(k.c_str(), n.c_str()) == 0) return str(v);
        return 0;
    });
    method(RQ, "HTTPBody", [](id self, SEL) -> id {
        auto& b = objc::ensure<RequestData>(self).body;
        return b.empty() ? 0 : data_with(b.data(), b.size());
    });
    method(RQ, "copyWithZone:", [](id self, SEL, u64) { return objc::retain(self); });
    method(RQ, "mutableCopyWithZone:", [](id self, SEL, u64) { return objc::retain(self); });
    method(MRQ, "setHTTPMethod:", [](id self, SEL, id m) { objc::ensure<RequestData>(self).method = utf8(m); });
    method(MRQ, "setHTTPBody:", [](id self, SEL, id body) { objc::ensure<RequestData>(self).body = data_bytes(body); });
    method(MRQ, "setURL:", [](id self, SEL, id url) {
        auto& d = objc::ensure<RequestData>(self);
        objc::retain(url);
        objc::release(d.url);
        d.url = url;
    });
    method(MRQ, "setAllHTTPHeaderFields:", [](id self, SEL, id fields) {
        auto& d = objc::ensure<RequestData>(self);
        for (auto& [k, v] : dict_items(fields)) set_header(d, utf8(k), utf8(v));
    });
    method(MRQ, "setHTTPBodyStream:", [](id, SEL, id) {});
    method(MRQ, "setValue:forHTTPHeaderField:", [](id self, SEL, id value, id name) {
        set_header(objc::ensure<RequestData>(self), utf8(name), utf8(value));
    });
    method(MRQ, "addValue:forHTTPHeaderField:", [](id self, SEL, id value, id name) {
        set_header(objc::ensure<RequestData>(self), utf8(name), utf8(value));
    });
    method(MRQ, "setTimeoutInterval:", [](id, SEL, double) {});
    method(MRQ, "setCachePolicy:", [](id, SEL, u64) {});
    method(MRQ, "setHTTPShouldHandleCookies:", [](id, SEL, bool) {});
    method(MRQ, "setHTTPShouldUsePipelining:", [](id, SEL, bool) {});

    Class CONN = objc::host_class("NSURLConnection");
    struct ConnData : objc::HostData {
        id delegate = 0;
        id request = 0;
        bool cancelled = false, started = false;
    };
    // Answers on the connection's run loop, as a real connection would: the response, its data and
    // the end, or the offline error.
    auto start_connection = [](id conn) {
        auto& cd = objc::ensure<ConnData>(conn);
        if (cd.started) return;
        cd.started = true;
        auto response = std::make_shared<HttpResponse>();
        bool served = serve(cd.request, *response);
        objc::retain(conn);
        RunLoop::current().post([conn, response, served] {
            auto& d = objc::ensure<ConnData>(conn);
            auto has = [&](const char* s) { return d.delegate && objc::responds_to(d.delegate, objc::sel(s)); };
            if (!d.cancelled && served) {
                if (has("connection:didReceiveResponse:"))
                    objc::send(d.delegate, "connection:didReceiveResponse:", {conn, make_response(d.request, *response)});
                if (!response->body.empty() && has("connection:didReceiveData:"))
                    objc::send(d.delegate, "connection:didReceiveData:", {conn, data_with(response->body.data(), response->body.size())});
                if (!d.cancelled && has("connectionDidFinishLoading:")) objc::send(d.delegate, "connectionDidFinishLoading:", {conn});
            } else if (!d.cancelled && has("connection:didFailWithError:")) {
                objc::send(d.delegate, "connection:didFailWithError:", {conn, offline_error()});
            }
            objc::release(conn);
        });
    };
    static decltype(start_connection) s_start = start_connection;
    method(CONN, "initWithRequest:delegate:", [](id self, SEL, id req, id delegate) {
        auto& d = objc::ensure<ConnData>(self);
        d.delegate = objc::retain(delegate);
        d.request = objc::retain(req);
        s_start(self);
        return self;
    });
    method(CONN, "initWithRequest:delegate:startImmediately:", [](id self, SEL, id req, id delegate, bool start) {
        auto& d = objc::ensure<ConnData>(self);
        d.delegate = objc::retain(delegate);
        d.request = objc::retain(req);
        if (start) s_start(self);
        return self;
    });
    class_method(CONN, "connectionWithRequest:delegate:", [](Class c, SEL, id req, id delegate) {
        return objc::autorelease(objc::send(objc::alloc(c), "initWithRequest:delegate:", {req, delegate}));
    });
    method(CONN, "start", [](id self, SEL) { s_start(self); });
    method(CONN, "cancel", [](id self, SEL) { objc::ensure<ConnData>(self).cancelled = true; });
    method(CONN, "scheduleInRunLoop:forMode:", [](id, SEL, id, id) {});
    method(CONN, "setDelegateQueue:", [](id, SEL, id) {});
    class_method(CONN, "sendSynchronousRequest:returningResponse:error:", [](Class, SEL, id req, u64* resp, u64* err) -> id {
        HttpResponse r;
        if (serve(req, r)) {
            if (resp) *resp = make_response(req, r);
            if (err) *err = 0;
            return data_with(r.body.data(), r.body.size());
        }
        if (resp) *resp = 0;
        if (err) *err = offline_error();
        return 0;
    });
    class_method(CONN, "sendAsynchronousRequest:queue:completionHandler:", [](Class, SEL, id req, id, GuestAddr block) {
        GuestAddr b = objc::block_copy(block);
        auto r = std::make_shared<HttpResponse>();
        bool served = serve(req, *r);
        objc::retain(req);
        post_to_main([b, r, served, req] {
            if (served) objc::call_block(b, {make_response(req, *r), data_with(r->body.data(), r->body.size()), 0});
            else objc::call_block(b, {0, 0, offline_error()});
            objc::block_release(b);
            objc::release(req);
        });
    });
    Class COOK = objc::host_class("NSHTTPCookieStorage");
    class_method(COOK, "sharedHTTPCookieStorage", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    method(COOK, "cookies", [](id, SEL) { return array({}); });
    method(COOK, "cookiesForURL:", [](id, SEL, id) { return array({}); });
    method(COOK, "deleteCookie:", [](id, SEL, id) {});
    method(COOK, "setCookie:", [](id, SEL, id) {});
    method(COOK, "setCookieAcceptPolicy:", [](id, SEL, u64) {});
    Class RESP = objc::host_class("NSURLResponse");
    Class HRESP = objc::host_class("NSHTTPURLResponse", "NSURLResponse");
    method(RESP, "URL", [](id self, SEL) { return objc::ensure<ResponseData>(self).url; });
    method(RESP, "expectedContentLength", [](id self, SEL) -> s64 { return (s64)objc::ensure<ResponseData>(self).length; });
    method(RESP, "MIMEType", [](id self, SEL) { return str(objc::ensure<ResponseData>(self).content_type); });
    method(RESP, "textEncodingName", [](id, SEL) { return str("utf-8"); });
    method(HRESP, "statusCode", [](id self, SEL) -> s64 { return objc::ensure<ResponseData>(self).status; });
    method(HRESP, "allHeaderFields", [](id self, SEL) {
        auto& d = objc::ensure<ResponseData>(self);
        return dict({{str("Content-Type"), str(d.content_type)}, {str("Content-Length"), str(std::to_string(d.length))}});
    });
    objc::host_class("NSHTTPCookie");

    // ---- archiving / JSON: not supported (callers treat nil as "no cached data") ----
    Class KA = objc::host_class("NSKeyedArchiver");
    Class KU = objc::host_class("NSKeyedUnarchiver");
    class_method(KA, "archivedDataWithRootObject:", [](Class, SEL, id o) {
        std::string x = plist_to_xml(o);
        return data_with(x.data(), x.size());
    });
    class_method(KA, "archiveRootObject:toFile:", [](Class, SEL, id o, id path) {
        std::string x = plist_to_xml(o);
        return objc::send(data_with(x.data(), x.size()), "writeToFile:atomically:", {path, 1}) != 0;
    });
    class_method(KU, "unarchiveObjectWithData:", [](Class, SEL, id data) -> id {
        auto b = data_bytes(data);
        id o = plist_from_xml(std::string(b.begin(), b.end()));
        return o ? objc::autorelease(o) : 0;
    });
    class_method(KU, "unarchiveObjectWithFile:", [](Class, SEL, id path) -> id {
        id data = objc::send(objc::class_named("NSData"), "dataWithContentsOfFile:", {path});
        if (!data) return 0;
        auto b = data_bytes(data);
        id o = plist_from_xml(std::string(b.begin(), b.end()));
        return o ? objc::autorelease(o) : 0;
    });
    Class JS = objc::host_class("NSJSONSerialization");
    class_method(JS, "isValidJSONObject:", [](Class, SEL, id) { return true; });
    class_method(JS, "JSONObjectWithData:options:error:", [](Class, SEL, id, u64, u64* err) -> id {
        LOG_WARN("NSJSONSerialization JSONObjectWithData: not implemented");
        if (err) *err = 0;
        return 0;
    });
    class_method(JS, "dataWithJSONObject:options:error:", [](Class, SEL, id o, u64, u64* err) {
        if (err) *err = 0;
        std::string s = objc::describe(o);
        return data_with(s.data(), s.size());
    });

    // ---- formatters / calendar: minimal ----
    Class NF = objc::host_class("NSNumberFormatter");
    for (const char* s : {"setNumberStyle:", "setFormatterBehavior:", "setMaximumFractionDigits:", "setMinimumFractionDigits:"})
        method(NF, s, [](id, SEL, u64) {});
    method(NF, "setLocale:", [](id, SEL, id) {});
    method(NF, "stringFromNumber:", [](id, SEL, id n) { return objc::send(n, "description"); });
    method(NF, "numberFromString:", [](id, SEL, id s) { return number_double(std::strtod(utf8(s).c_str(), nullptr)); });
    Class DF = objc::host_class("NSDateFormatter");
    for (const char* s : {"setDateFormat:", "setLocale:", "setTimeZone:", "setCalendar:"}) method(DF, s, [](id, SEL, id) {});
    for (const char* s : {"setDateStyle:", "setTimeStyle:"}) method(DF, s, [](id, SEL, u64) {});
    method(DF, "stringFromDate:", [](id, SEL, id d) { return objc::send(d, "description"); });
    method(DF, "dateFromString:", [](id, SEL, id) { return objc::send(objc::class_named("NSDate"), "date"); });
    Class CAL = objc::host_class("NSCalendar");
    class_method(CAL, "currentCalendar", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    method(CAL, "initWithCalendarIdentifier:", [](id self, SEL, id) { return self; });
    method(CAL, "components:fromDate:", [](id, SEL, u64, id) {
        return objc::autorelease(objc::alloc(objc::host_class("NSDateComponents")));
    });
    Class DC = objc::host_class("NSDateComponents");
    for (const char* s : {"year", "month", "day", "hour", "minute", "second", "weekday"}) method(DC, s, [](id, SEL) -> s64 { return 1; });
}

}  // namespace ns
