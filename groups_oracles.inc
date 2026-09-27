/* Independent architectural checks, derived from Intel Vol. 2 FDIV,
   FST/FSTP and FPREM/FPREM1, and AMD 21928 PFMIN/PFMAX.
   Expected bit patterns are constants, not results borrowed from 86Box. */
static uint16_t oracle_cw;
static uint32_t oracle_wait_ip;
#ifndef HOSTTEST
static uint32_t oracle_saved_cr0;
#endif
static void
oracle_before(const struct variant *v)
{
#ifndef HOSTTEST
    __asm__ volatile("mov %%cr0, %0" : "=r"(oracle_saved_cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(oracle_saved_cr0 | 0x20) : "memory");
#endif
}
static void
oracle_after(const struct variant *v)
{
    __asm__ volatile("fninit" ::: "memory");
#ifndef HOSTTEST
    __asm__ volatile("mov %0, %%cr0" : : "r"(oracle_saved_cr0) : "memory");
#endif
}
static const struct oracle_div_case {
    uint32_t num_lo, num_hi, den_lo, den_hi, result_lo, result_hi, exception, c1;
} div_cases[] = {
    {0,0,          0,0,          0,0xfff80000,1}, /* 0/0: invalid, not ZE */
    {0,0x3ff00000, 0,0,          0,0x7ff00000,4}, /* 1/+0 */
    {0,0x3ff00000, 0,0x80000000, 0,0xfff00000,4}, /* 1/-0 */
    {0,0x7ff00000, 0,0x7ff00000, 0,0xfff80000,1}, /* inf/inf */
    {0,0x7ff00000, 0,0,          0,0x7ff00000,0}, /* inf/0: no ZE */
    {0,0x7ff80000, 0,0,          0,0x7ff80000,0}, /* QNaN/0: no ZE */
    {0,0,          0,0x40000000, 0,0,0},
    {0,0x40080000, 0,0x40000000, 0,0x3ff80000,0}, /* 3/2 */
    {0,0x40000000, 0,0x40080000, 0x55555555,0x3fe55555,0x20,0x200}, /* 2/3, PC64 rounds upward */
};
static void
fix_div_oracle(const struct variant *v)
{
    const struct oracle_div_case *c = &div_cases[v->arg];
    g_in.ebx = (uint32_t) g_buf;
    put_le(g_in.buf, c->num_lo, 4); put_le(g_in.buf+4, c->num_hi, 4);
    put_le(g_in.buf+8, c->den_lo, 4); put_le(g_in.buf+12, c->den_hi, 4);
    oracle_cw = 0x037f & ~(v->arg2 ? c->exception : 0);
}
static void
oracle_init(struct emit *e)
{
    ebytes(e,(const uint8_t[]){0xdb,0xe3,0xd9,0x2d},4); /* fninit; fldcw */
    e32(e,(uint32_t)&oracle_cw);
}
static void
oracle_status(struct emit *e)
{
    ebytes(e,(const uint8_t[]){0xd9,0x35},2); e32(e,(uint32_t)g_fenv);
    ebytes(e,(const uint8_t[]){0x0f,0xb7,0x3d},3); e32(e,(uint32_t)&g_fenv[1]);
    ebytes(e,(const uint8_t[]){0x0f,0xb7,0x35},3); e32(e,(uint32_t)&g_fenv[2]);
    ebytes(e,(const uint8_t[]){0xdb,0xe2},2); /* fnclex after snapshot */
}
static void
emit_div_oracle(struct emit *e,const struct variant *v)
{
    oracle_init(e);
    ebytes(e,(const uint8_t[]){0xdd,0x43,8,0xdd,0x03},5); /* denominator, numerator */
    ebytes(e,v->target,v->target_len);
    if (v->arg3) {oracle_wait_ip=(uint32_t)e->p;e8(e,0x9b);}
    oracle_status(e);
    ebytes(e,(const uint8_t[]){0xdd,0x5b,32,0xdb,0xe3},5); /* result; clear for next case */
}
static uint32_t
oracle_u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static const char *
expect_div(const struct variant *v,const struct kin *in,const struct kout *o)
{
    const struct oracle_div_case *c=&div_cases[v->arg];
    int unmasked=v->arg2 && c->exception;
    if(v->arg3) {
        if(o->fault!=16 || o->fault_err || o->fault_ip!=oracle_wait_ip-(uint32_t)g_cur_slot)
            return "unmasked FDIV must retire, then #MF at FWAIT";
        if(memcmp_(o->buf+32,in->buf+32,8)) return "faulting FWAIT allowed later result store";
        return 0;
    }
    if(o->fault!=0xff) return "FDIV unexpectedly trapped before FWAIT";
    if((o->edi&0x23f)!=(c->exception|c->c1)) return "FDIV exception class (IE versus ZE)";
    if((o->edi&0x8080)!=(unmasked?0x8080:0)) return "FDIV pending exception summary";
    if((o->edi&0x3800)!=0x3000) return "FDIV changed TOP";
    int suppressed=unmasked && c->exception!=0x20; /* unmasked precision still writes */
    uint32_t lo=suppressed?c->num_lo:c->result_lo,hi=suppressed?c->num_hi:c->result_hi;
    if(oracle_u32(o->buf+32)!=lo || oracle_u32(o->buf+36)!=hi) return "FDIV numeric result or unmasked destination suppression";
    return 0;
}
static int
dm_oracle_x87(const struct variant *v,const struct kin *in,struct kout *o,uint16_t *undef)
{
    o->edi &= ~0x4700u; /* condition bits checked explicitly where specified */
    return 1;
}
static const struct empty_case {uint8_t esc,modrm,size,pop;uint32_t lo,hi,ex;} empty_cases[]={
    {0xd9,0x53,4,0,0xffc00000,0,0}, {0xd9,0x5b,4,1,0xffc00000,0,0},
    {0xdd,0x53,8,0,0,0xfff80000,0}, {0xdd,0x5b,8,1,0,0xfff80000,0},
    {0xdb,0x7b,10,1,0,0xc0000000,0xffff},
    {0xdf,0x53,2,0,0x8000,0,0}, {0xdf,0x5b,2,1,0x8000,0,0},
    {0xdb,0x53,4,0,0x80000000,0,0}, {0xdb,0x5b,4,1,0x80000000,0,0},
    {0xdf,0x7b,8,1,0,0x80000000,0},
    {0xdf,0x73,10,1,0,0xc0000000,0xffff}, /* FBSTP integer indefinite */
};
static void
fix_empty_oracle(const struct variant *v)
{
    g_in.ebx=(uint32_t)g_buf; memset(g_in.buf+32,0xa5,16);
    oracle_cw=v->arg2?0x037e:0x037f;
}
static void
emit_empty_oracle(struct emit *e,const struct variant *v)
{
    oracle_init(e);
    /* Seed a stale payload, then empty the stack: a stale register must
       never be mistaken for a valid source by an empty store. */
    ebytes(e,(const uint8_t[]){0xd9,0xe8,0xdd,0xc0},4); /* fld1; ffree st0 */
    ebytes(e,v->target,v->target_len);
    oracle_status(e);
    ebytes(e,(const uint8_t[]){0xdb,0xe3},2);
}
static const char *
expect_empty(const struct variant *v,const struct kin *in,const struct kout *o)
{
    const struct empty_case *c=&empty_cases[v->arg];
    if(o->fault!=0xff) return "empty store unexpectedly trapped before FWAIT";
    if((o->edi&0x027f)!=0x0041) return "empty store must set IE/SF and clear C1";
    if((o->edi&0x8080)!=(v->arg2?0x8080:0)) return "empty store exception summary";
    unsigned top=(!v->arg2 && c->pop)?0:7; /* FLD1 made TOP=7 */
    if(((o->edi>>11)&7)!=top || o->esi!=0xffff) return "empty store TOP/tag update";
    uint8_t expected[16]; memset(expected,0xa5,sizeof(expected));
    if(!v->arg2) {
        put_le(expected,c->lo,c->size<4?c->size:4);
        if(c->size>=8) put_le(expected+4,c->hi,4);
        if(c->size==10) put_le(expected+8,c->ex,2);
    }
    if(memcmp_(expected,o->buf+32,sizeof(expected))) return "empty store indefinite, canary, or unmasked write suppression";
    return 0;
}
static void
fix_remainder_oracle(const struct variant *v)
{
    g_in.ebx=(uint32_t)g_buf; oracle_cw=0x037f;
    put_le(g_in.buf,0,4);put_le(g_in.buf+4,v->arg?0x46300000:0x401c0000,4); /* 2^100 or 7 */
    put_le(g_in.buf+8,0,4);put_le(g_in.buf+12,v->arg?0x40080000:0x40000000,4); /* 3 or 2 */
}
static void
emit_remainder_oracle(struct emit *e,const struct variant *v)
{
    oracle_init(e); ebytes(e,(const uint8_t[]){0xdd,0x43,8,0xdd,0x03,0xb9},6);e32(e,16);
    uint8_t *again=e->p;
    ebytes(e,v->target,v->target_len);
    ebytes(e,(const uint8_t[]){0xdf,0xe0,0xf6,0xc4,4,0x74,2,0xe2},8); /* SW.C2; jz done; loop again */
    e8(e,(uint8_t)(again-(e->p+1)));
    oracle_status(e); ebytes(e,(const uint8_t[]){0xdd,0x5b,32,0xdb,0xe3},5);
}
static const char *
expect_remainder(const struct variant *v,const struct kin *in,const struct kout *o)
{
    unsigned q=v->arg?5:v->arg2?4:3;
    unsigned cc=((q&4)?0x100:0)|((q&2)?0x4000:0)|((q&1)?0x200:0);
    if(o->fault!=0xff || !o->ecx || (o->edi&0x473f)!=cc) return "completed remainder exception/C2/quotient bits";
    if(oracle_u32(o->buf+32)!=0 || oracle_u32(o->buf+36)!=((!v->arg&&v->arg2)?0xbff00000:0x3ff00000)) return "completed remainder value";
    return 0;
}
static void
x87_arch_cases(const char *name)
{
    struct variant v={.group=name,.fx=FX_CUSTOM,.defmask=dm_oracle_x87,.norm=1<<R_EBX,
        .fix_input=fix_div_oracle,.emit=emit_div_oracle,.expect=expect_div,.before_run=oracle_before,.after_run=oracle_after};
    v.target[0]=0xd8;v.target[1]=0xf1;v.target_len=2;
    for(v.arg=0;v.arg<sizeof(div_cases)/sizeof(div_cases[0]);v.arg++)
        for(v.arg2=0;v.arg2<2;v.arg2++)run_keep(&v,1);
    v.arg2=1;v.arg3=1;v.faults_ok=1;
    for(v.arg=0;v.arg<4;v.arg++)run_keep(&v,1);
    v.arg3=0;v.faults_ok=0;
    v.fix_input=fix_empty_oracle;v.emit=emit_empty_oracle;v.expect=expect_empty;
    for(v.arg=0;v.arg<sizeof(empty_cases)/sizeof(empty_cases[0]);v.arg++) {
        v.target[0]=empty_cases[v.arg].esc;v.target[1]=empty_cases[v.arg].modrm;v.target[2]=32;v.target_len=3;
        for(v.arg2=0;v.arg2<2;v.arg2++)run_keep(&v,1);
    }
    v.fix_input=fix_remainder_oracle;v.emit=emit_remainder_oracle;v.expect=expect_remainder;
    for(v.arg=0;v.arg<2;v.arg++)for(v.arg2=0;v.arg2<2;v.arg2++) {
        v.target[0]=0xd9;v.target[1]=v.arg2?0xf5:0xf8;v.target_len=2;run_keep(&v,1);
    }
}
/* Mutation checks: a consistently wrong answer must fail its predicate. */
static void
oracle_selfcheck(void)
{
    struct variant v={0};struct kin in={0};struct kout o={0};
    v.arg=0;o.fault=0xff;o.edi=0x3001;put_le(o.buf+36,0xfff80000,4);
    if(expect_div(&v,&in,&o)) {puts_("ORACLE SELFTEST: correct FDIV rejected\n");halt_forever();}
    o.edi=0x3004;
    if(!expect_div(&v,&in,&o)) {puts_("ORACLE SELFTEST: wrong FDIV accepted\n");halt_forever();}
    memset(&o,0,sizeof(o));o.fault=0xff;o.edi=0x3841;o.esi=0xffff;
    memset(o.buf+32,0xa5,16);put_le(o.buf+32,0xffc00000,4);
    if(expect_empty(&v,&in,&o)) {puts_("ORACLE SELFTEST: correct empty store rejected\n");halt_forever();}
    put_le(o.buf+32,0x3f800000,4);
    if(!expect_empty(&v,&in,&o)) {puts_("ORACLE SELFTEST: stale empty store accepted\n");halt_forever();}
    puts_("ORACLE SELFTEST PASS\n");
}

static const struct {uint32_t a,b,min,max;} minmax_cases[]={
    {0x80000000,0,0,0},{0,0x80000000,0,0},{0x80000000,0x80000000,0,0},
    {0x80000000,0x3f800000,0,0x3f800000},
    {0xbf800000,0x80000000,0xbf800000,0},
    {0x3f800000,0xbf800000,0xbf800000,0x3f800000},
    /* AMD 21928 table 1: exponent-zero inputs are zero, including
       nonzero fraction bits. Tables 11/12 require positive result zero. */
    {0x00000001,0x80000001,0,0},
    {0x007fffff,0x3f800000,0,0x3f800000},
    {0xbf800000,0x807fffff,0xbf800000,0},
};
static const uint32_t rsqrt_inputs[]={
    0,0x80000000,0x40800000,0xc0800000,0x00000001,0x80000001
};
static void
fix_3dnow_oracle(const struct variant *v)
{
    for(int i=0;i<2;i++) {
        g_mmx_in[i]=minmax_cases[v->arg].a;
        g_mmx_in[2+i]=minmax_cases[v->arg].b;
    }
    if(v->arg3) g_mmx_in[2]=rsqrt_inputs[v->arg];
}
static const char *
expect_3dnow(const struct variant *v,const struct kin *in,const struct kout *o)
{
    if(o->fault!=0xff) return "3DNow documented operand unexpectedly faulted";
    if(v->arg3) {
        uint32_t bits=o->mmx[0],sign=(v->arg&1)?0x80000000u:0;
        if(o->mmx[1]!=bits || (bits&0x80000000u)!=sign) return "PFRSQRT lane replication/sign";
        bits&=0x7fffffff;
        if(!(rsqrt_inputs[v->arg]&0x7f800000)) {if(bits!=0x7f7fffff) return "PFRSQRT exponent-zero input must saturate to maximum normal";}
        else if(bits<0x3f000000-512 || bits>0x3f000000+512) return "PFRSQRT approximation outside documented precision";
    } else {
        uint32_t want=v->arg2?minmax_cases[v->arg].max:minmax_cases[v->arg].min;
        if(o->mmx[0]!=want || o->mmx[1]!=want) return "PFMIN/PFMAX value or required positive zero";
    }
    return 0;
}
static void
threednow_arch_cases(const char *name)
{
    struct variant v={.group=name,.mmx=1,.fx=FX_NONE,.fix_input=fix_3dnow_oracle,.reg_fix=1,.expect=expect_3dnow};
    v.target[0]=0x0f;v.target[1]=0x0f;v.target[2]=0xc1;v.target_len=4;
    for(v.arg=0;v.arg<sizeof(minmax_cases)/sizeof(minmax_cases[0]);v.arg++)
        for(v.arg2=0;v.arg2<2;v.arg2++){v.target[3]=v.arg2?0xa4:0x94;run_keep(&v,1);}
    v.arg3=1;v.target[3]=0x97;
    /* The estimate's exact payload is model-dependent, but its bounds,
       sign, zero behavior, and lane replication have explicit predicates. */
    v.fx=FX_UNDEF;
    for(v.arg=0;v.arg<sizeof(rsqrt_inputs)/sizeof(rsqrt_inputs[0]);v.arg++)run_keep(&v,1);
}
static const char *
expect_emms(const struct variant *v,const struct kin *in,const struct kout *o)
{
    if(o->fault!=0xff || o->esi!=0xffff || (o->edi&0x3800)!=0) return "EMMS must empty tags and reset TOP (Intel Vol. 1, 8.5.4.1)";
    return 0;
}
static void
emms_arch_cases(const char *name)
{
    struct variant v={.group=name,.fx=FX_CUSTOM,.defmask=dm_oracle_x87,.expect=expect_emms};
    const uint8_t bytes[]={0xdb,0xe3,0xd9,0xe8,0x0f,0x77}; /* fninit; fld1; emms */
    memcpy(v.target,bytes,sizeof(bytes));v.target_len=put_fenv(v.target,sizeof(bytes));run_keep(&v,1);
}

/* Result-projection regression checks use deliberately different raw
   observations and independently check that defined changes still fail. */
static void
projection_selfcheck(void)
{
    struct variant v={.fx=FX_CUSTOM,.fx_arg=1};
    struct kin in={0};struct kout a={0},b={0},pa,pb;
    const defmask_fn masks[]={dm_exhaust,dm_sample_loop,dm_fxarg};
    for(unsigned i=0;i<3;i++) {
        v.defmask=masks[i];memset(&a,0,sizeof(a));b=a;
        if(i==0)b.ebx=F_AF<<16;
        if(i==1)b.eax=0x3f123456;
        if(i==2)b.flags=F_AF;
        if(!defined_result(&v,&in,&a,&pa)||!defined_result(&v,&in,&b,&pb)||memcmp_(&pa,&pb,sizeof(pa))) {
            puts_("PROJECTION SELFTEST: undefined observation leaked\n");halt_forever();
        }
        b.ebp^=1;
        defined_result(&v,&in,&b,&pb);
        if(!memcmp_(&pa,&pb,sizeof(pa))) {puts_("PROJECTION SELFTEST: defined result hidden\n");halt_forever();}
    }
    v.defmask=dm_3dnow;v.arg3=1;v.target[0]=0x0f;v.target[1]=0x0e;
    memset(&a,0,sizeof(a));b=a;b.mmx[3]=0xdeadbeef;
    defined_result(&v,&in,&a,&pa);defined_result(&v,&in,&b,&pb);
    if(memcmp_(&pa,&pb,sizeof(pa))) {puts_("PROJECTION SELFTEST: FEMMS payload leaked\n");halt_forever();}
    b.esi=0xffff;defined_result(&v,&in,&b,&pb);
    if(!memcmp_(&pa,&pb,sizeof(pa))) {puts_("PROJECTION SELFTEST: tag word hidden\n");halt_forever();}
    puts_("PROJECTION SELFTEST PASS\n");
}
