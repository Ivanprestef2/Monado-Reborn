// IPC message parsing/serialization and session management.
#include "diag/diag.h"
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "ipc.h"

#ifdef __APPLE__
extern const ServiceDef __start_services[] __asm__("section$start$__DATA$__services");
extern const ServiceDef __stop_services[] __asm__("section$end$__DATA$__services");
#elif defined(__linux__)
extern const ServiceDef __start_services[], __stop_services[];  // (ELF: defined by the linker)
#else
// PE/COFF: .svc$a < .svc$m (entries, ipc.h) < .svc$z (the linker sorts by the text after '$')
__attribute__((section(".svc$a"), used)) static const ServiceDef __start_services[1];
__attribute__((section(".svc$z"), used)) static const ServiceDef __stop_services[1];
#endif

// [XCDE] T283: protect session ownership, service references and shared domain slots.
// Service handlers and close callbacks run outside this lock.
static pthread_mutex_t ipc_lock = PTHREAD_MUTEX_INITIALIZER;

static void stub_handler(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[ipc] STUB %s cmd %u (in %u bytes, x%d a%d b%d c%d) -> ok\n", s->name, req->cmd, req->data_size, req->nx,
        req->na, req->nb, req->nc);
    diag_unimplemented("ipc", "[stub service] %s cmd %u", s->name, req->cmd);  // [XCDE] T033
    resp->result = 0;
    resp_size(resp, 0x40);  // zeroed outputs
}

Service *service_new(const char *name, ServiceHandler h, void *state) {
    Service *s = calloc(1, sizeof *s);
    s->name = strdup(name);
    s->handler = h;
    s->state = state;
    s->refs = 1;
    return s;
}

Service *service_lookup(const char *name) {
    const ServiceDef *start = __start_services, *stop = __stop_services;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const ServiceDef *d = start; d < stop; d++)
        if (d->name && !strcmp(d->name, name)) return service_new(name, d->handler, NULL);
    LOG("[ipc] no HLE implementation for service '%s', using logging stub\n", name);
    diag_hle_hit("service", name);
    return service_new(name, stub_handler, NULL);
}

void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }

// Takes ownership of both references.
static uint32_t session_new(Service *s, Domain *domain) {
    KSession *k = calloc(1, sizeof *k);
    k->obj.type = KOBJ_SESSION;
    k->svc = s;
    k->domain = domain;
    return kernel_new_handle(&k->obj);
}

static void service_release(Service *s) {
    if (!s) return;
    pthread_mutex_lock(&ipc_lock);
    bool last = --s->refs == 0;
    pthread_mutex_unlock(&ipc_lock);
    if (last) {
        if (s->on_close) s->on_close(s);
        free((void *)s->name);
        free(s);
    }
}

static void domain_release(Domain *domain) {
    if (!domain) return;
    pthread_mutex_lock(&ipc_lock);
    bool last = --domain->refs == 0;
    pthread_mutex_unlock(&ipc_lock);
    if (!last) return;
    for (int i = 1; i < 256; i++) service_release(domain->objs[i]);
    free(domain);
}

static void session_close(KSession *session) {
    pthread_mutex_lock(&ipc_lock);
    Service *svc = session->svc;
    Domain *domain = session->domain;
    session->svc = NULL;
    session->domain = NULL;
    pthread_mutex_unlock(&ipc_lock);
    domain_release(domain);
    service_release(svc);
}

void ipc_session_destroy(KObject *object) {
    KSession *session = (KSession *)object;
    session_close(session);
    free(session);
}

uint32_t ipc_connect_named_port(const char *name) {
    if (!strcmp(name, "sm:")) return session_new(service_lookup("sm:"), NULL);
    LOG("[ipc] ConnectToNamedPort('%s') unsupported\n", name);
    return 0;
}

// ------------------------------------------------------------------ sm:
SERVICE("sm:", sm_handler) {
    (void)s;
    switch (req->cmd) {
    case 0:  // RegisterClient
        break;
    case 1: {  // GetServiceHandle(name)
        char name[9] = {0};
        memcpy(name, req->data, 8);
        LOG("[sm] GetService %s\n", name);
        ipc_return_object(resp, service_lookup(name));
        break;
    }
    default:
        LOG("[sm] unknown cmd %u\n", req->cmd);
    }
}

// ------------------------------------------------------------------ message codec
static uint64_t buf_addr(const uint32_t *w) {
    return w[1] | ((uint64_t)(w[2] >> 28) << 32) | ((uint64_t)((w[2] >> 2) & 7) << 36);
}

static int handle_request(KSession *sess, Service *svc, Domain *domain, uint32_t *msg, uint32_t handle) {
    IpcReq req = {0};
    IpcResp resp = {0};
    req.type = msg[0] & 0xFFFF;
    int nx = (msg[0] >> 16) & 0xF, na = (msg[0] >> 20) & 0xF, nb = (msg[0] >> 24) & 0xF;
    uint32_t raw_words = msg[1] & 0x3FF;
    uint32_t cflags = (msg[1] >> 10) & 0xF;
    uint32_t *p = msg + 2;
    if (msg[1] & 0x80000000u) {
        uint32_t hd = *p++;
        if (hd & 1) { req.pid = p[0] | ((uint64_t)p[1] << 32); p += 2; }
        req.ncopy = (hd >> 1) & 0xF;
        req.nmove = (hd >> 5) & 0xF;
        for (int i = 0; i < req.ncopy; i++) req.copy_handles[i] = *p++;
        for (int i = 0; i < req.nmove; i++) req.move_handles[i] = *p++;
    }
    for (int i = 0; i < nx; i++, p += 2) {
        req.x[i].addr = p[1] | ((uint64_t)((p[0] >> 12) & 0xF) << 32) | ((uint64_t)((p[0] >> 6) & 0x7) << 36);
        req.x[i].size = p[0] >> 16;
    }
    req.nx = nx;
    for (int i = 0; i < na; i++, p += 3) {
        req.a[i].size = p[0] | ((uint64_t)((p[2] >> 24) & 0xF) << 32);
        req.a[i].addr = buf_addr(p);
    }
    req.na = na;
    for (int i = 0; i < nb; i++, p += 3) {
        req.b[i].size = p[0] | ((uint64_t)((p[2] >> 24) & 0xF) << 32);
        req.b[i].addr = buf_addr(p);
    }
    req.nb = nb;
    uint32_t *raw = p;
    // raw data is aligned to 16 bytes relative to the message start
    uint32_t *aligned = (uint32_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    if (cflags >= 2) {
        uint32_t *cdesc = raw + raw_words;
        int ncd = cflags == 2 ? 1 : (int)cflags - 2;
        for (int i = 0; i < ncd; i++) {
            req.c[i].addr = cdesc[2 * i] | ((uint64_t)(cdesc[2 * i + 1] & 0xFFFF) << 32);
            req.c[i].size = cdesc[2 * i + 1] >> 16;
        }
        req.nc = ncd;
    }

    uint8_t *payload = (uint8_t *)aligned;
    bool dom = false;  // response is domain-wrapped
    if (req.type == 4 || req.type == 6) {
        if (domain) {
            dom = true;
            uint8_t dcmd = payload[0];
            uint8_t nin = payload[1];
            uint16_t dsize = *(uint16_t *)(payload + 2);
            req.domain_obj = *(uint32_t *)(payload + 4);
            if (dcmd == 2) {  // close object
                if (req.domain_obj < 256) {
                    pthread_mutex_lock(&ipc_lock);
                    Service *o = domain->objs[req.domain_obj];
                    domain->objs[req.domain_obj] = NULL;
                    pthread_mutex_unlock(&ipc_lock);
                    service_release(o);
                }
                goto write_response;
            }
            req.ndomain_in = nin;
            for (int i = 0; i < nin; i++) req.domain_in_objs[i] = *(uint32_t *)(payload + 16 + dsize + 4 * i);
            payload += 16;
        }
        if (memcmp(payload, "SFCI", 4)) {
            LOG("[ipc] bad SFCI magic on %s (domain=%d):", svc->name, domain != NULL);
            for (int i = 0; i < 24; i++) LOG(" %08x", msg[i]);
            LOG("\n");
        }
        req.cmd = *(uint32_t *)(payload + 8);
        req.data = payload + 16;
        req.data_size = raw_words * 4 > 32 ? raw_words * 4 - 32 : 0;
        Service *target = svc;
        if (domain) {
            pthread_mutex_lock(&ipc_lock);
            target = req.domain_obj < 256 ? domain->objs[req.domain_obj] : NULL;
            if (target) target->refs++;
            pthread_mutex_unlock(&ipc_lock);
            if (!target) { LOG("[ipc] domain object %u missing\n", req.domain_obj); return 0xE401; }
        }
        if (diag_usage_on()) diag_use("ipc", "%s cmd %u", target->name, req.cmd);  // [XCDE] T034
        target->handler(target, &req, &resp);
        if (domain) service_release(target);
    } else if (req.type == 5 || req.type == 7) {  // control
        req.cmd = *(uint32_t *)(payload + 8);
        LOG("[ipc] control %u on %s (handle %x)\n", req.cmd, svc->name, handle);
        switch (req.cmd) {
        case 0:  // ConvertCurrentObjectToDomain
            pthread_mutex_lock(&ipc_lock);
            if (!sess->svc) {
                pthread_mutex_unlock(&ipc_lock);
                return 0xE401;
            }
            if (!sess->domain) {
                sess->domain = calloc(1, sizeof(Domain));
                sess->domain->refs = 1;
                sess->domain->objs[1] = svc;
                svc->refs++;
            }
            pthread_mutex_unlock(&ipc_lock);
            resp_size(&resp, 4);
            *(uint32_t *)resp.data = 1;
            break;
        case 2: case 4: {  // CloneCurrentObject(Ex)
            pthread_mutex_lock(&ipc_lock);
            svc->refs++;
            if (domain) domain->refs++;
            pthread_mutex_unlock(&ipc_lock);
            uint32_t h = session_new(svc, domain);
            resp.move_handles[resp.nmove++] = h;
            break;
        }
        case 3:  // QueryPointerBufferSize
            *(uint16_t *)resp.data = 0x8000;
            resp_size(&resp, 4);
            break;
        default:
            LOG("[ipc] control cmd %u\n", req.cmd);
        }
    } else {
        LOG("[ipc] message type %u\n", req.type);
    }

write_response:;
    // returned objects: domain objects or new sessions
    uint32_t domain_out[8];
    int ndomain_out = 0;
    if (dom) {
        pthread_mutex_lock(&ipc_lock);
        for (int id = 1; id < 256 && ndomain_out < resp.nobjects; id++)
            if (!domain->objs[id]) domain_out[ndomain_out++] = id;
        if (ndomain_out == resp.nobjects) {
            for (int i = 0; i < ndomain_out; i++) domain->objs[domain_out[i]] = resp.objects[i];
            pthread_mutex_unlock(&ipc_lock);
        } else {
            pthread_mutex_unlock(&ipc_lock);
            for (int i = 0; i < resp.nobjects; i++) service_release(resp.objects[i]);
            ndomain_out = 0;
            resp.result = 0xD001;
        }
    } else {
        for (int i = 0; i < resp.nobjects; i++) {
            resp.move_handles[resp.nmove++] = session_new(resp.objects[i], NULL);
        }
    }
    memset(msg, 0, 0x100);
    uint32_t *w = msg;
    int has_handles = resp.ncopy || resp.nmove;
    uint32_t payload_words = 4 + (resp.data_size + 3) / 4;   // SFCO header + data
    if (dom) payload_words += 4 + ndomain_out;
    uint32_t raw_size = payload_words + 4;  // + alignment padding
    w[0] = 0;  // type 0 response
    w[1] = raw_size | (has_handles ? 0x80000000u : 0);
    uint32_t *q = w + 2;
    if (has_handles) {
        *q++ = (resp.ncopy << 1) | (resp.nmove << 5);
        for (int i = 0; i < resp.ncopy; i++) *q++ = resp.copy_handles[i];
        for (int i = 0; i < resp.nmove; i++) *q++ = resp.move_handles[i];
    }
    uint32_t *out = (uint32_t *)(((uintptr_t)q + 15) & ~(uintptr_t)15);
    if (dom) {
        out[0] = ndomain_out;
        out += 4;
    }
    memcpy(out, "SFCO", 4);
    out[1] = 0;
    out[2] = resp.result;
    out[3] = 0;
    memcpy(out + 4, resp.data, resp.data_size);
    if (dom)
        for (int i = 0; i < ndomain_out; i++) out[4 + (resp.data_size + 3) / 4 + i] = domain_out[i];
    return 0;
}

int ipc_handle_request(Cpu *c, uint32_t handle) {
    KObject *object = kernel_session_acquire(handle);
    if (!object) {
        LOG("[ipc] SendSyncRequest on bad handle %x\n", handle);
        return 0xE401;
    }
    KSession *sess = (KSession *)object;
    uint32_t *msg = (uint32_t *)(uintptr_t)c->tpidrro;
    int result = 0;
    if ((msg[0] & 0xFFFF) == 2) {
        session_close(sess);
    } else {
        pthread_mutex_lock(&ipc_lock);
        Service *svc = sess->svc;
        Domain *domain = sess->domain;
        if (svc) svc->refs++;
        if (domain) domain->refs++;
        pthread_mutex_unlock(&ipc_lock);
        result = svc ? handle_request(sess, svc, domain, msg, handle) : 0xE401;
        domain_release(domain);
        service_release(svc);
    }
    kernel_session_release(object);
    return result;
}
