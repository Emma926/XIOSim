/* fetch-STM.cpp - Simple(r) Timing Model */
/*
 * __COPYRIGHT__ GT
 */

#ifdef ZESTO_PARSE_ARGS
  if(!strcasecmp(fetch_opt_string,"STM"))
    return std::make_unique<class core_fetch_STM_t>(core);
#else

class core_fetch_STM_t:public core_fetch_t
{
  enum fetch_stall_t {FSTALL_byteQ_FULL, /* byteQ is full */
                      FSTALL_TBR,      /* predicted taken */
                      FSTALL_EOL,      /* hit end of cache line */
                      FSTALL_SYSCALL,  /* syscall waiting for pipe to clear */
                      FSTALL_ZPAGE,    /* fetch request from zeroth page of memory */
                      FSTALL_ORACLE,   /* oracle stall on MopQ capacity */
                      FSTALL_num
                     };

  public:

  /* constructor, stats registration */
  core_fetch_STM_t(struct core_t * const core);
  ~core_fetch_STM_t();
  virtual void reg_stats(xiosim::stats::StatsDatabase* sdb);
  virtual void update_occupancy(void);

  /* simulate one cycle */
  virtual void pre_fetch(void);
  virtual bool do_fetch(void);
  virtual void post_fetch(void);


  /* decode interface */
  virtual bool Mop_available(void);
  virtual struct Mop_t * Mop_peek(void);
  virtual void Mop_consume(void);

  /* enqueue a jeclear event into the jeclear_pipe */
  virtual void jeclear_enqueue(struct Mop_t * const Mop, const md_addr_t New_PC);
  /* recover the front-end after the recover request actually
     reaches the front end. */
  virtual void recover(const md_addr_t new_PC); 

  protected:

  int byteQ_num;

  /* The byteQ contains the raw bytes (well, the addresses thereof).
     This is similar to the old IFQ in that the contents of the I$
     are delivered here.  Each entry contains raw bytes, and so
     this may correspond to many, or even less than one, instruction
     per entry. */
  struct byteQ_entry_t {
    md_addr_t addr;
    tick_t when_fetch_requested;
    tick_t when_fetched;
    tick_t when_translation_requested;
    tick_t when_translated;
    int num_Mop;
    int MopQ_first_index;
    seq_t action_id;
    struct core_t * core;
  } * byteQ;
  int byteQ_linemask; /* for masking out line offset */
  int byteQ_head;
  int byteQ_tail;

  static const char *fetch_stall_str[FSTALL_num];

  void byteQ_request(const md_addr_t lineaddr);
  bool byteQ_is_full(void);
  bool byteQ_already_requested(const md_addr_t addr);

  static void IL1_callback(void * const op);
  static void ITLB_callback(void * const op);
  static bool translated_callback(void * const op, const seq_t action_id);
  static seq_t get_byteQ_action_id(void * const op);

  enum fetch_stall_t stall_reason;
};

const char *core_fetch_STM_t::fetch_stall_str[FSTALL_num] = {
  "byteQ full             ",
  "taken branch           ",
  "end of cache line      ",
  "trap waiting on drain  ",
  "request for page zero  ",
  "oracle stall on MopQ   "
};


core_fetch_STM_t::core_fetch_STM_t(struct core_t * const arg_core):
  byteQ_num(0)
{
  struct core_knobs_t * knobs = arg_core->knobs;
  core = arg_core;

  /* must come after exec_init()! */

  /* bpred */
  bpred = std::make_unique<bpred_t>(
      core,
      knobs->fetch.num_bpred_components,
      knobs->fetch.bpred_opt_str,
      knobs->fetch.fusion_opt_str,
      knobs->fetch.dirjmpbtb_opt_str,
      knobs->fetch.indirjmpbtb_opt_str,
      knobs->fetch.ras_opt_str
    );

  zesto_assert(knobs->fetch.jeclear_delay == 0, void);

  create_caches();

  byteQ = (byteQ_entry_t*) calloc(knobs->fetch.byteQ_size,sizeof(*byteQ));
  byteQ_head = byteQ_tail = 0;
  if(!byteQ)
    fatal("couldn't calloc byteQ");

  byteQ_linemask = ~(md_addr_t)(knobs->fetch.byteQ_linesize - 1);
}

core_fetch_STM_t::~core_fetch_STM_t() {
    free(byteQ);
}

void
core_fetch_STM_t::reg_stats(xiosim::stats::StatsDatabase* sdb)
{
    int coreID = core->id;

    stat_reg_note(sdb, "\n#### BPRED STATS ####");
    bpred->reg_stats(sdb, core);

    stat_reg_note(sdb, "\n#### INST CACHE STATS ####");
    cache_reg_stats(sdb, core, core->memory.IL1.get());
    cache_reg_stats(sdb, core, core->memory.ITLB.get());

    stat_reg_note(sdb, "\n#### FETCH STATS ####");
    auto sim_cycle_st = stat_find_core_stat<tick_t>(sdb, coreID, "sim_cycle");
    assert(sim_cycle_st);
    auto& fetch_insn_st = stat_reg_core_counter(sdb, true, coreID, "fetch_insn",
                                                "total number of instructions fetched",
                                                &core->stat.fetch_insn, 0, true, NULL);
    auto& fetch_uops_st =
            stat_reg_core_counter(sdb, true, coreID, "fetch_uops", "total number of uops fetched",
                                  &core->stat.fetch_uops, 0, true, NULL);
    stat_reg_core_formula(sdb, true, coreID, "fetch_IPC", "IPC at fetch",
                          fetch_insn_st / *sim_cycle_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "fetch_uPC", "uPC at fetch",
                          fetch_uops_st / *sim_cycle_st, NULL);

    core->stat.fetch_stall = stat_reg_core_dist(
            sdb, coreID, "fetch_stall", "breakdown of stalls in fetch", 0, FSTALL_num,
            (PF_COUNT | PF_PDF), NULL, fetch_stall_str, true, NULL);

    auto& byteQ_occupancy_st = stat_reg_core_counter(sdb, false, coreID, "byteQ_occupancy",
                                                     "total byteQ occupancy (in lines/entries)",
                                                     &core->stat.byteQ_occupancy, 0, true, NULL);

    stat_reg_core_formula(sdb, true, coreID, "byteQ_avg", "average byteQ occupancy (in insts)",
                          byteQ_occupancy_st / *sim_cycle_st, NULL);
}

void core_fetch_STM_t::update_occupancy(void)
{
  core->stat.byteQ_occupancy += byteQ_num;
}


/******************************/
/* byteQ/I$ RELATED FUNCTIONS */
/******************************/

bool core_fetch_STM_t::byteQ_is_full(void)
{
  struct core_knobs_t * knobs = core->knobs;
  return (byteQ_num >= knobs->fetch.byteQ_size);
}

/* is this line already the most recently requested? */
bool core_fetch_STM_t::byteQ_already_requested(const md_addr_t addr)
{
  struct core_knobs_t * knobs = core->knobs;
  int index = moddec(byteQ_tail,knobs->fetch.byteQ_size); //(byteQ_tail-1+knobs->fetch.byteQ_size) % knobs->fetch.byteQ_size;
  if(byteQ_num && (byteQ[index].addr == (addr & byteQ_linemask)))
    return true;
  else
    return false;
}

/* Initiate a fetch request */
void core_fetch_STM_t::byteQ_request(const md_addr_t lineaddr)
{
  struct core_knobs_t * knobs = core->knobs;
  /* this function assumes you already called byteQ_already_requested so that
     you don't double-request the same line */
  byteQ[byteQ_tail].when_fetch_requested = TICK_T_MAX;
  byteQ[byteQ_tail].when_fetched = TICK_T_MAX;
  byteQ[byteQ_tail].when_translation_requested = TICK_T_MAX;
  byteQ[byteQ_tail].when_translated = TICK_T_MAX;
  byteQ[byteQ_tail].addr = lineaddr;
  byteQ[byteQ_tail].action_id = core->new_action_id();
  byteQ[byteQ_tail].core = core;
  byteQ_tail = modinc(byteQ_tail,knobs->fetch.byteQ_size); //(byteQ_tail+1)%knobs->fetch.byteQ_size;
  byteQ_num++;

  return;
}

/* Callback functions used by the cache code to indicate when the IL1/ITLB lookups
   have been completed. */
void core_fetch_STM_t::IL1_callback(void * const op)
{
  byteQ_entry_t * byteQ = (byteQ_entry_t*) op;
  byteQ->when_fetched = byteQ->core->sim_cycle;
}

void core_fetch_STM_t::ITLB_callback(void * const op)
{
  byteQ_entry_t * byteQ = (byteQ_entry_t*) op;
  byteQ->when_translated = byteQ->core->sim_cycle;
}

bool core_fetch_STM_t::translated_callback(void * const op, const seq_t action_id)
{
  byteQ_entry_t * byteQ = (byteQ_entry_t*) op;
  if(byteQ->action_id == action_id)
    return byteQ->when_translated <= byteQ->core->sim_cycle;
  else
    return true;
}

seq_t core_fetch_STM_t::get_byteQ_action_id(void * const op)
{
  byteQ_entry_t * byteQ = (byteQ_entry_t*) op;
  return byteQ->action_id;
}

/************************/
/* MAIN FETCH FUNCTIONS */
/************************/
void core_fetch_STM_t::pre_fetch(void)
{
  struct core_knobs_t * knobs = core->knobs;
  int asid = core->asid;
  int i;

  ZESTO_STAT(stat_add_sample(core->stat.fetch_stall, (int)stall_reason);)

  /* check byteQ and send requests to I$/ITLB */
  int index = byteQ_head;
  for(i=0;i<byteQ_num;i++)
  {
    if(byteQ[index].when_fetch_requested == TICK_T_MAX)
    {
      if(cache_enqueuable(core->memory.IL1.get(), asid, byteQ[index].addr))
      {
        cache_enqueue(core, core->memory.IL1.get(), NULL, CACHE_READ, asid, byteQ[index].addr, byteQ[index].addr, byteQ[index].action_id, 0, NO_MSHR, &byteQ[index], IL1_callback, NULL, translated_callback, get_byteQ_action_id);
        byteQ[index].when_fetch_requested = core->sim_cycle;
        break;
      }
    }
    index = modinc(index,knobs->fetch.byteQ_size);
  }

  index = byteQ_head;
  for(i=0;i<byteQ_num;i++)
  {
    if(byteQ[index].when_translation_requested == TICK_T_MAX)
    {
      if(cache_enqueuable(core->memory.ITLB.get(), asid, memory::page_table_address(asid, byteQ[index].addr)))
      {
        cache_enqueue(core, core->memory.ITLB.get(), NULL, CACHE_READ, 0, asid, memory::page_table_address(asid, byteQ[index].addr), byteQ[index].action_id, 0, NO_MSHR, &byteQ[index], ITLB_callback, NULL, NULL, get_byteQ_action_id);
        byteQ[index].when_translation_requested = core->sim_cycle;
        break;
      }
    }
    index = modinc(index,knobs->fetch.byteQ_size);
  }
}

void core_fetch_STM_t::post_fetch(void)
{
  struct core_knobs_t * knobs = core->knobs;

  /* This gets processed here, so that demand misses from the DL1 get higher
     priority for accessing the L2 */
  cache_process(core->memory.ITLB.get());
  cache_process(core->memory.IL1.get());

  /* XXX: I don't think we really should need this code here, since
     whenever a Mop gets consumed, this check should be happening
     any way (see the Mop_consume function)... but the simulator
     seems to get stuck with a line where all Mops have been read
     yet has not been reclaimed. */
  if(byteQ_num && (byteQ[byteQ_head].when_fetched != TICK_T_MAX) 
                  && (byteQ[byteQ_head].when_translated != TICK_T_MAX)
                  && (byteQ[byteQ_head].num_Mop <= 0))
  {
    /* consumed all insts from this fetch line */
    byteQ[byteQ_head].MopQ_first_index = -1;
    byteQ[byteQ_head].when_fetch_requested = TICK_T_MAX;
    byteQ[byteQ_head].when_fetched = TICK_T_MAX;
    byteQ[byteQ_head].when_translation_requested = TICK_T_MAX;
    byteQ[byteQ_head].when_translated = TICK_T_MAX;
    byteQ_num--;
    byteQ_head = modinc(byteQ_head,knobs->fetch.byteQ_size); //(byteQ_head+1)%knobs->fetch.byteQ_size;
  }

  stall_reason = FSTALL_EOL;
}

bool core_fetch_STM_t::do_fetch(void)
{
  struct core_knobs_t * knobs = core->knobs;
  md_addr_t current_line = PC & byteQ_linemask;
  struct Mop_t * Mop = NULL;
  
  /* Waiting for pipe to clear from system call/trap. */
  if (core->oracle->is_draining()) {
    stall_reason = FSTALL_SYSCALL;
    return false;
  }

  Mop = core->oracle->exec(PC);

  if(Mop && (memory::page_round_down(PC) == 0)) {
    zesto_assert(core->oracle->spec_mode,false);
    stall_reason = FSTALL_ZPAGE;
    return false;
  }

  if(!Mop) {
    stall_reason = FSTALL_ORACLE;
    return false;
  }


  /* We explicitly check for both the address of the first byte and the last
     byte, since x86 instructions have no alignment restrictions and therefore
     may end up getting split across more than one cache line.  If so, this
     can generate more than one IL1/ITLB lookup. */
  if(byteQ_already_requested(Mop->fetch.PC))
  {
    Mop->fetch.first_byte_requested = true;
    if(Mop->timing.when_fetch_started == TICK_T_MAX)
      Mop->timing.when_fetch_started = core->sim_cycle;
  }
  if(!Mop->fetch.first_byte_requested)
  {
    if(byteQ_is_full()) /* stall if byteQ is full */
    {
      stall_reason = FSTALL_byteQ_FULL;
      return false;
    }

    if(Mop->timing.when_fetch_started == TICK_T_MAX)
      Mop->timing.when_fetch_started = core->sim_cycle;
    Mop->fetch.first_byte_requested = true;
    byteQ_request(Mop->fetch.PC & byteQ_linemask);
  }

  /* STM model doesn't deal with fetches across cache lines */
  Mop->fetch.last_byte_requested = true;

  zesto_assert(Mop->fetch.first_byte_requested,false);

  /* All bytes for this Mop have been requested.  Record it in the byteQ entry
     and let the oracle know we're done with it so can proceed to the next
     one */

  int byteQ_index = moddec(byteQ_tail,knobs->fetch.byteQ_size); //(byteQ_tail-1+knobs->fetch.byteQ_size)%knobs->fetch.byteQ_size;
  if(byteQ[byteQ_index].num_Mop == 0)
    byteQ[byteQ_index].MopQ_first_index = core->oracle->get_index(Mop);
  byteQ[byteQ_index].num_Mop++;

  core->oracle->consume(Mop);

  /* figure out where to fetch from next */
  if(Mop->decode.is_ctrl || Mop->decode.has_rep)
  {
    Mop->fetch.bpred_update = bpred->get_state_cache();

    Mop->fetch.pred_NPC = bpred->lookup(Mop->fetch.bpred_update,
    Mop->decode.opflags, Mop->fetch.PC,Mop->fetch.PC+Mop->fetch.len,Mop->decode.targetPC,
    Mop->oracle.NextPC,(Mop->oracle.NextPC != (Mop->fetch.PC+Mop->fetch.len)));


    bpred->spec_update(Mop->fetch.bpred_update,Mop->decode.opflags,
    Mop->fetch.PC,Mop->decode.targetPC,Mop->oracle.NextPC,Mop->fetch.bpred_update->our_pred);
  }
  else
    Mop->fetch.pred_NPC = Mop->fetch.PC + Mop->fetch.len;

  if(Mop->fetch.pred_NPC != Mop->oracle.NextPC)
  {
    Mop->oracle.recover_inst = true;
    Mop->uop[Mop->decode.last_uop_index].oracle.recover_inst = true;
  }

  /* advance the fetch PC to the next instruction */
  PC = Mop->fetch.pred_NPC;

  if(  (Mop->fetch.pred_NPC != (Mop->fetch.PC + Mop->fetch.len))
    && (Mop->fetch.pred_NPC != Mop->fetch.PC)) /* REPs don't count as taken branches w.r.t. fetching */
  {
    stall_reason = FSTALL_TBR;
    return false;
  }
  else if((Mop->fetch.PC & byteQ_linemask) != current_line)
  {
    stall_reason = FSTALL_EOL;
    return false;
  }
  else if(core->oracle->is_draining())
  {
    stall_reason = FSTALL_SYSCALL;
    return false;
  }

  /* still fetching from the same byteQ entry */
  return ((PC & byteQ_linemask) == current_line);
}

/*void core_fetch_STM_t::step(void)
{
   post_fetch();
   while(do_fetch());
   pre_fetch();
}*/

bool core_fetch_STM_t::Mop_available(void)
{
  return byteQ_num && byteQ[byteQ_head].when_fetched != TICK_T_MAX
                   && byteQ[byteQ_head].when_translated != TICK_T_MAX
                   && byteQ[byteQ_head].num_Mop;
}

struct Mop_t * core_fetch_STM_t::Mop_peek(void)
{
  int MopQ_index = byteQ[byteQ_head].MopQ_first_index;
  zesto_assert(MopQ_index != -1,NULL);
  return core->oracle->get_Mop(MopQ_index);
}

/* check byteQ for fetched/translated instructions */
void core_fetch_STM_t::Mop_consume(void)
{
  struct core_knobs_t * knobs = core->knobs;
  
  /* assumes you alerady called Mop_available to check that
     a ready Mop exists. */
  int MopQ_index = byteQ[byteQ_head].MopQ_first_index;
  struct Mop_t * Mop = core->oracle->get_Mop(MopQ_index);

  Mop->timing.when_fetched = core->sim_cycle;
  if(Mop->uop[Mop->decode.last_uop_index].decode.EOM)
    ZESTO_STAT(core->stat.fetch_insn++;)

  ZESTO_STAT(core->stat.fetch_uops += Mop->stat.num_uops;)

  byteQ[byteQ_head].num_Mop--;
  byteQ[byteQ_head].MopQ_first_index = core->oracle->next_index(MopQ_index);

  if(byteQ[byteQ_head].num_Mop <= 0)
  {
    /* consumed all insts from this fetch line */
    byteQ[byteQ_head].MopQ_first_index = -1;
    byteQ[byteQ_head].when_fetch_requested = TICK_T_MAX;
    byteQ[byteQ_head].when_fetched = TICK_T_MAX;
    byteQ[byteQ_head].when_translation_requested = TICK_T_MAX;
    byteQ[byteQ_head].when_translated = TICK_T_MAX;
    byteQ_num--;
    byteQ_head = modinc(byteQ_head,knobs->fetch.byteQ_size); //(byteQ_head+1)%knobs->fetch.byteQ_size;
  }
}

void core_fetch_STM_t::jeclear_enqueue(struct Mop_t * const Mop, const md_addr_t New_PC)
{
  /* does nothing; should not get called for STM */
  fatal("core_fetch_SMT_t::jeclear_enqueue should never get called");
}

void
core_fetch_STM_t::recover(const md_addr_t new_PC)
{
  struct core_knobs_t * knobs = core->knobs;
  /* XXX: use non-oracle nextPC as there may be multiple re-fetches */
  int i;
  PC = new_PC;

  /* clear out the byteQ */
  for(i=0;i<knobs->fetch.byteQ_size;i++)
  {
    byteQ[i].addr = 0;
    byteQ[i].when_fetch_requested = TICK_T_MAX;
    byteQ[i].when_fetched = TICK_T_MAX;
    byteQ[i].when_translation_requested = TICK_T_MAX;
    byteQ[i].when_translated = TICK_T_MAX;
    byteQ[i].num_Mop = 0;
    byteQ[i].MopQ_first_index = -1;
    byteQ[i].action_id = core->new_action_id();
    byteQ[i].core = core;
  }
  byteQ_num = 0;
  byteQ_head = 0;
  byteQ_tail = 0;
}

#endif
