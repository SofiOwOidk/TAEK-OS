/* Constructores que usa la workqueue original, despues de kthread_runtime.inc.
 * kernel/kthread.c se compila integro; sus dos constructores de fork/kthreadd quedan
 * renombrados a *_linux, sin llamadas, y estos crean una Task+fibra TaekOS reales. */
#undef kthread_create_on_node
#undef kthread_create_worker

static struct task_struct *kt_crear_con_formato(int (*fn)(void *), void *data, unsigned autoreap,
                                                const char *fmt, va_list ap) {
    char nombre[64];
    if (vsnprintf(nombre, sizeof(nombre), fmt, ap) >= (int)sizeof(nombre)) return ERR_PTR(-ENAMETOOLONG);
    unsigned long id = 0;
    int rc = taek_kthread_crear(fn, data, nombre, &id);
    if (rc) return ERR_PTR(rc);
    struct taek_kthread_record *r = kt_find(id);
    if (!r) kt_fatal(120);
    r->autoreap = autoreap;
    return r->task;
}

/* Los workers de la workqueue terminan solos (WORKER_DIE) y nadie hace kthread_stop:
 * el exit de Linux los recogeria. Aqui lo hace la raiz cuando sale la fibra. */
struct task_struct *kthread_create_on_node(int (*fn)(void *), void *data, int node, const char fmt[], ...) {
    (void)node;
    va_list ap;
    va_start(ap, fmt);
    struct task_struct *t = kt_crear_con_formato(fn, data, 1, fmt, ap);
    va_end(ap);
    return t;
}

/* Mismo cuerpo que __kthread_create_worker original, sin afinidad. El kthread_worker
 * se detiene con kthread_destroy_worker (kthread_stop), asi que no se autorrecoge. */
struct kthread_worker *kthread_create_worker(unsigned int flags, const char fmt[], ...) {
    struct kthread_worker *worker = kzalloc(sizeof(*worker), GFP_KERNEL);
    if (!worker) return ERR_PTR(-ENOMEM);
    kthread_init_worker(worker);
    va_list ap;
    va_start(ap, fmt);
    struct task_struct *task = kt_crear_con_formato(kthread_worker_fn, worker, 0, fmt, ap);
    va_end(ap);
    if (IS_ERR(task)) {
        kfree(worker);
        return ERR_CAST(task);
    }
    worker->flags = flags;
    worker->task = task;
    wake_up_process(task);
    return worker;
}

/* Recogida de lo que ya termino. Una retencion se reintenta en la siguiente vuelta. */
int taek_kthreads_autorrecoger(void) {
    if (!kt_root() || kt.serving) return -EINVAL;
    int hechos = 0;
    for (unsigned i = 0; i < TAEK_TASKS_SLOTS; i++) {
        struct taek_kthread_record *r = &kt.records[i];
        if (!r->autoreap || (r->phase != KT_EXITED && r->phase != KT_REAP) || r->joining || r->request) continue;
        if (r->task && taek_tasks_refs(r->id) > 1) continue;
        if (r->fiber) {
            int state = kt.ops.estado(r->fiber);
            if (state != -9 && state != 1 && state != 5 && state != 6) continue;
        }
        r->phase = KT_REAP;
        if (!kt_reap(r)) { r->autoreap = 0; hechos++; }
    }
    return hechos;
}
