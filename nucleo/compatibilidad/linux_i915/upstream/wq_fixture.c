/* Ejercita la workqueue original con el contrato que necesita i915_driver_early_probe:
 * las tres colas de i915_workqueues_init, orden, exclusion y progreso bajo bloqueo. */
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include "../upstream_wq.h"

static struct taek_wq_resultado res;
static int subsistema_listo;
#define COMPROBAR(c, bit) do { res.checks++; if (!(c)) res.mascara |= (1ul << (bit)); } while (0)

static void dormir_jiffies(long n) { schedule_timeout_interruptible(n); }

/* ---- orden y exclusion en una cola ordenada ---- */
struct prueba_orden {
    struct work_struct w;
    int indice, *siguiente, *activos, *errores;
};
static void trabajo_orden(struct work_struct *ws) {
    struct prueba_orden *p = container_of(ws, struct prueba_orden, w);
    if (*p->siguiente != p->indice) (*p->errores)++;
    (*p->siguiente)++;
    if (++(*p->activos) != 1) (*p->errores)++;
    cond_resched();             /* Cede a mitad: otro de la misma cola no debe entrar. */
    (*p->activos)--;
    res.ejecuciones++;
}
static void probar_ordenada(struct workqueue_struct *wq) {
    struct prueba_orden t[8];
    int siguiente = 0, activos = 0, errores = 0;
    for (int i = 0; i < 8; i++) {
        INIT_WORK(&t[i].w, trabajo_orden);
        t[i].indice = i; t[i].siguiente = &siguiente; t[i].activos = &activos; t[i].errores = &errores;
        COMPROBAR(queue_work(wq, &t[i].w), 0);
    }
    flush_workqueue(wq);
    COMPROBAR(siguiente == 8, 1);
    COMPROBAR(errores == 0, 2);
}

/* ---- una ordenada bloqueada no frena a la otra ---- */
struct bloqueada { struct work_struct w; struct completion *soltar, *entro; };
static void trabajo_bloqueado(struct work_struct *ws) {
    struct bloqueada *b = container_of(ws, struct bloqueada, w);
    complete(b->entro);
    wait_for_completion(b->soltar);
    res.ejecuciones++;
}
struct simple { struct work_struct w; struct completion *hecho; };
static void trabajo_simple(struct work_struct *ws) {
    struct simple *s = container_of(ws, struct simple, w);
    complete(s->hecho);
    res.ejecuciones++;
}
static void probar_independencia(struct workqueue_struct *a, struct workqueue_struct *b) {
    DECLARE_COMPLETION_ONSTACK(entro);
    DECLARE_COMPLETION_ONSTACK(soltar);
    DECLARE_COMPLETION_ONSTACK(hecho);
    struct bloqueada bq = {.soltar = &soltar, .entro = &entro};
    struct simple sp = {.hecho = &hecho};
    INIT_WORK_ONSTACK(&bq.w, trabajo_bloqueado);
    INIT_WORK_ONSTACK(&sp.w, trabajo_simple);
    queue_work(a, &bq.w);
    wait_for_completion(&entro);
    queue_work(b, &sp.w);
    COMPROBAR(wait_for_completion_timeout(&hecho, 500) != 0, 3);
    complete(&soltar);
    flush_workqueue(a);
    flush_workqueue(b);
    destroy_work_on_stack(&bq.w);
    destroy_work_on_stack(&sp.w);
}

/* ---- A espera a B en el mismo pool: exige wq_worker_sleeping/running reales ---- */
struct espera_ab { struct work_struct a, b; struct completion c; int orden; };
static void trabajo_b(struct work_struct *ws) {
    struct espera_ab *p = container_of(ws, struct espera_ab, b);
    p->orden = 1;
    complete(&p->c);
}
static void trabajo_a(struct work_struct *ws) {
    struct espera_ab *p = container_of(ws, struct espera_ab, a);
    wait_for_completion(&p->c);
    if (p->orden == 1) p->orden = 2;
}
static void probar_a_espera_b(struct workqueue_struct *wq) {
    struct espera_ab p = {.orden = 0};
    init_completion(&p.c);
    INIT_WORK_ONSTACK(&p.a, trabajo_a);
    INIT_WORK_ONSTACK(&p.b, trabajo_b);
    queue_work(wq, &p.a);
    queue_work(wq, &p.b);
    flush_workqueue(wq);
    COMPROBAR(p.orden == 2, 4);
    destroy_work_on_stack(&p.a);
    destroy_work_on_stack(&p.b);
}

/* ---- autoliberacion, doble encolado y autorrearme ---- */
static int autolibres;
static void trabajo_se_libera(struct work_struct *ws) { kfree(ws); autolibres++; }
static void probar_autolibre(struct workqueue_struct *wq) {
    autolibres = 0;
    for (int i = 0; i < 4; i++) {
        struct work_struct *ws = kmalloc(sizeof(*ws), GFP_KERNEL);
        COMPROBAR(ws != NULL, 5);
        if (!ws) return;
        INIT_WORK(ws, trabajo_se_libera);
        queue_work(wq, ws);
    }
    flush_workqueue(wq);
    COMPROBAR(autolibres == 4, 6);
}
struct rearme { struct work_struct w; struct workqueue_struct *wq; int veces, limite; };
static void trabajo_rearme(struct work_struct *ws) {
    struct rearme *r = container_of(ws, struct rearme, w);
    if (++r->veces < r->limite) queue_work(r->wq, ws);
}
static void probar_doble_y_rearme(struct workqueue_struct *wq) {
    struct rearme r = {.wq = wq, .limite = 16};
    INIT_WORK_ONSTACK(&r.w, trabajo_rearme);
    COMPROBAR(queue_work(wq, &r.w), 7);
    bool segundo = queue_work(wq, &r.w);   /* Sin ceder: el primero sigue pendiente. */
    COMPROBAR(!segundo, 8);
    while (r.veces < r.limite) dormir_jiffies(1);
    flush_work(&r.w);
    COMPROBAR(r.veces == r.limite, 9);
    destroy_work_on_stack(&r.w);
}

/* ---- cancelacion de pendiente, en curso y que se rearma ---- */
struct cancelable { struct work_struct w; struct workqueue_struct *wq; int veces; bool rearmar; };
static void trabajo_cancelable(struct work_struct *ws) {
    struct cancelable *c = container_of(ws, struct cancelable, w);
    c->veces++;
    cond_resched();
    if (c->rearmar) queue_work(c->wq, ws);
}
static void probar_cancelacion(struct workqueue_struct *wq) {
    struct cancelable c = {.wq = wq};
    INIT_WORK_ONSTACK(&c.w, trabajo_cancelable);
    queue_work(wq, &c.w);
    COMPROBAR(cancel_work_sync(&c.w), 10);     /* Pendiente: se cancela sin ejecutarse. */
    COMPROBAR(c.veces == 0, 11);
    c.rearmar = true;
    queue_work(wq, &c.w);
    while (c.veces < 3) dormir_jiffies(1);
    cancel_work_sync(&c.w);                    /* Contrato: ni pendiente ni en curso. */
    int visto = c.veces;
    COMPROBAR(!work_pending(&c.w), 12);
    dormir_jiffies(8);
    COMPROBAR(c.veces == visto, 13);
    destroy_work_on_stack(&c.w);
}

/* ---- trabajo retrasado: necesita timer_list real ---- */
struct retrasado { struct delayed_work d; struct completion c; };
static void trabajo_retrasado(struct work_struct *ws) {
    struct retrasado *r = container_of(to_delayed_work(ws), struct retrasado, d);
    complete(&r->c);
}
static void probar_retrasado(struct workqueue_struct *wq) {
    struct retrasado r;
    init_completion(&r.c);
    INIT_DELAYED_WORK_ONSTACK(&r.d, trabajo_retrasado);
    unsigned long antes = jiffies;
    queue_delayed_work(wq, &r.d, 5);
    COMPROBAR(wait_for_completion_timeout(&r.c, 250) != 0, 14);
    COMPROBAR(time_after_eq(jiffies, antes + 5), 15);    /* No se adelanto. */
    reinit_completion(&r.c);
    queue_delayed_work(wq, &r.d, 500);
    COMPROBAR(cancel_delayed_work_sync(&r.d), 16);
    dormir_jiffies(8);
    COMPROBAR(!completion_done(&r.c), 17);
    destroy_delayed_work_on_stack(&r.d);
}

/* ---- las tres colas de i915_workqueues_init, varias rondas ---- */
int taek_wq_fixture_probar(void) {
    memset(&res, 0, sizeof(res));
    if (!subsistema_listo) {
        pr_info("etapa: workqueue_init_early\n");
        workqueue_init_early();
        pr_info("etapa: workqueue_init\n");
        workqueue_init();
        pr_info("etapa: subsistema listo\n");
        subsistema_listo = 1;
    }
    for (int ronda = 0; ronda < 3; ronda++) {
        pr_info("etapa: ronda %d\n", ronda);
        struct workqueue_struct *i915 = alloc_ordered_workqueue("i915", 0);
        struct workqueue_struct *dp = alloc_ordered_workqueue("i915-dp", 0);
        struct workqueue_struct *un = alloc_workqueue("i915-unordered", 0, 0);
        COMPROBAR(i915 && dp && un, 18);
        if (!i915 || !dp || !un) break;
        probar_ordenada(i915);
        probar_ordenada(dp);
        probar_independencia(i915, dp);
        probar_a_espera_b(un);
        probar_autolibre(un);
        probar_doble_y_rearme(un);
        probar_cancelacion(un);
        probar_retrasado(un);
        destroy_workqueue(un);
        /* i915-dp comparte pool: debe seguir funcionando al destruir una ordenada. */
        destroy_workqueue(i915);
        probar_ordenada(dp);
        destroy_workqueue(dp);
        res.rondas++;
    }
    return res.mascara ? -EIO : 0;
}
void taek_wq_fixture_resultado(struct taek_wq_resultado *o) { if (o) *o = res; }
