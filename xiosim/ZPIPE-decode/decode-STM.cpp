/* decode-STM.cpp - Simple(r) Timing Model */
/*
 * __COPYRIGHT__ GT
 */

#ifdef ZESTO_PARSE_ARGS
  if(!strcasecmp(decode_opt_string,"STM"))
    return std::make_unique<class core_decode_STM_t>(core);
#else

class core_decode_STM_t:public core_decode_t
{
  enum decode_stall_t {DSTALL_NONE,   /* no stall */
                       DSTALL_FULL,   /* first decode stage is full */
                       DSTALL_EMPTY,  /* no insts to decode */
                       DSTALL_TARGET, /* target correction flushed remaining insts (only occurs when target_stage == crack_stage-1) */
                       DSTALL_num
                      };

  public:

  /* constructor, stats registration */
  core_decode_STM_t(struct core_t * const core);
  ~core_decode_STM_t();
  virtual void reg_stats(xiosim::stats::StatsDatabase* sdb);
  virtual void update_occupancy(void);

  virtual void step(void);
  virtual void recover(void);
  virtual void recover(struct Mop_t * const Mop);

  /* interface functions for alloc stage */
  virtual bool uop_available(void);
  virtual struct uop_t * uop_peek(void);
  virtual void uop_consume(void);

  protected:

  struct Mop_t *** pipe; /* the actual decode pipe */
  int * occupancy;

  static const char * decode_stall_str[DSTALL_num];

  enum decode_stall_t check_target(struct Mop_t * const Mop);
  bool check_flush(const int stage, const int idx);
};

const char * core_decode_STM_t::decode_stall_str[DSTALL_num] = {
  "no stall                   ",
  "next decode stage is full  ",
  "no insts to decode         ",
  "target correction          ",
};

core_decode_STM_t::core_decode_STM_t(struct core_t * const arg_core)
{
  struct core_knobs_t * knobs = arg_core->knobs;
  core = arg_core;

  if(knobs->decode.depth <= 0)
    fatal("decode pipe depth must be > 0");

  if(knobs->decode.width <= 0)
    fatal("decode pipe width must be > 0");

  if(knobs->decode.target_stage <= 0 || knobs->decode.target_stage >= knobs->decode.depth)
    fatal("decode target resteer stage (%d) must be > 0, and less than decode pipe depth (currently set to %d)",knobs->decode.target_stage,knobs->decode.depth);

  /* if the pipe is N wide, we assume there are N decoders */
  pipe = (struct Mop_t***) calloc(knobs->decode.depth,sizeof(*pipe));
  if(!pipe)
    fatal("couldn't calloc decode pipe");

  for(int i=0;i<knobs->decode.depth;i++)
  {
    pipe[i] = (struct Mop_t**) calloc(knobs->decode.width,sizeof(**pipe));
    if(!pipe[i])
      fatal("couldn't calloc decode pipe stage");
  }

  occupancy = (int*) calloc(knobs->decode.depth,sizeof(*occupancy));
  if(!occupancy)
    fatal("couldn't calloc decode pipe occupancy array");
}

core_decode_STM_t::~core_decode_STM_t() {
    free(occupancy);
    for (int i = 0; i < core->knobs->decode.depth; i++)
        free(pipe[i]);
    free(pipe);
}

void
core_decode_STM_t::reg_stats(xiosim::stats::StatsDatabase* sdb)
{
    int coreID = core->id;

    stat_reg_note(sdb, "\n#### DECODE STATS ####");
    stat_reg_core_counter(sdb, true, coreID, "target_resteers", "decode-time target resteers",
                          &core->stat.target_resteers, 0, true, NULL);
    auto& decode_insn_st = stat_reg_core_counter(sdb, true, coreID, "decode_insn",
                                                 "total number of instructions decodeed",
                                                 &core->stat.decode_insn, 0, true, NULL);
    auto& decode_uops_st = stat_reg_core_counter(sdb, true, coreID, "decode_uops",
                                                 "total number of uops decodeed",
                                                 &core->stat.decode_uops, 0, true, NULL);
    auto core_sim_cycles_st = stat_find_core_stat<tick_t>(sdb, coreID, "sim_cycle");
    assert(core_sim_cycles_st);

    stat_reg_core_formula(sdb, true, coreID, "decode_IPC", "IPC at decode",
                          decode_insn_st / *core_sim_cycles_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "decode_uPC", "uPC at decode",
                          decode_uops_st / *core_sim_cycles_st, NULL);

    core->stat.decode_stall = stat_reg_core_dist(
            sdb, coreID, "decode_stall", "breakdown of stalls at decode", 0, DSTALL_num,
            (PF_COUNT | PF_PDF), NULL, decode_stall_str, true, NULL);
}

void core_decode_STM_t::update_occupancy(void)
{
}

/*************************/
/* MAIN DECODE FUNCTIONS */
/*************************/

/* Helper functions to check for BAC-related resteers */

/* Check an individual Mop to see if it's next target is correct/reasonable, and
   recover if necessary. */
enum core_decode_STM_t::decode_stall_t
core_decode_STM_t::check_target(struct Mop_t * const Mop)
{
  if(Mop->decode.is_ctrl)
  {
    if((Mop->fetch.pred_NPC != Mop->fetch.ftPC) /* branch is predicted taken */
        || Mop->decode.opflags.UNCOND)
    {
      if(Mop->fetch.pred_NPC != Mop->decode.targetPC) /* wrong target */
      {
        if(Mop->fetch.bpred_update)
          core->fetch->bpred->recover(Mop->fetch.bpred_update,/*taken*/1);
        ZESTO_STAT(core->stat.target_resteers++;)
        core->oracle->recover(Mop);
        recover(Mop);
        core->fetch->recover(Mop->decode.targetPC);
        Mop->fetch.pred_NPC = Mop->decode.targetPC;
        return DSTALL_TARGET;
      }
    }
  }
  return DSTALL_NONE;
}

/* Perform branch-address calculation check for a given decode-pipe stage
   and Mop position. */
bool core_decode_STM_t::check_flush(const int stage, const int idx)
{
  struct core_knobs_t * knobs = core->knobs;
  /* stage-1 because we assume the following checks are acted upon only after
     the instruction has spent a cycle in the corresponding pipeline stage. */
  struct Mop_t * Mop = pipe[stage-1][idx];
  enum decode_stall_t stall_reason;

  if(Mop)
  {
    if((stage-1) == knobs->decode.target_stage)
    {
      if((stall_reason = check_target(Mop)))
      {
        pipe[stage][idx] = pipe[stage-1][idx];
        pipe[stage-1][idx] = NULL;
        if(pipe[stage][idx]) {
          occupancy[stage]++;
          occupancy[stage-1]--;
          zesto_assert(occupancy[stage] <= knobs->decode.width,false);
          zesto_assert(occupancy[stage-1] >= 0,false);
        }
        ZESTO_STAT(stat_add_sample(core->stat.decode_stall, (int)stall_reason);)
        return true;
      }
    }
  }

  return false;
}


/* shuffle Mops down the decode pipe, read Mops from fetch */
void core_decode_STM_t::step(void)
{
  struct core_knobs_t * knobs = core->knobs;
  int stage, i;
  enum decode_stall_t stall_reason = DSTALL_NONE;

  /* walk pipe backwards up to and but not including the first stage*/
  for(stage=knobs->decode.depth-1; stage > 0; stage--)
  {
    if(0 == occupancy[stage]) /* implementing non-serpentine pipe (no compressing) - can't advance until stage is empty */
    {
      zesto_assert(occupancy[stage] == 0,(void)0);

      /* move everyone from previous stage forward */
      if(occupancy[stage-1])
        for(i=0;i<knobs->decode.width;i++)
        {
          if(check_flush(stage,i))
            return;

          pipe[stage][i] = pipe[stage-1][i];
          pipe[stage-1][i] = NULL;
          if(pipe[stage][i]) {
            occupancy[stage]++;
            occupancy[stage-1]--;
            zesto_assert(occupancy[stage] <= knobs->decode.width,(void)0);
            zesto_assert(occupancy[stage-1] >= 0,(void)0);
          }
        }
    }
  }

  /* process the first stage, which reads Mops from fetch */
  if(!core->fetch->Mop_available())
  {
    stall_reason = DSTALL_EMPTY;
  }
  else if(occupancy[0] == 0) /* non-serpentine pipe; only decode if first stage is empty */
  {
    int Mops_decoded = 0;
    for(i=0;(i<knobs->decode.width) && core->fetch->Mop_available();i++)
    {
      if(pipe[0][i] == NULL) /* decoder available */
      {
        struct Mop_t * fetch_Mop = core->fetch->Mop_peek();

        /* consume the Mop from fetch */
        pipe[0][i] = fetch_Mop;
        occupancy[0]++;
        zesto_assert(occupancy[0] <= knobs->decode.width,(void)0);
        core->fetch->Mop_consume();
        Mops_decoded++;
      }
    }
    if(Mops_decoded == 0)
      stall_reason = DSTALL_FULL;
  }

  ZESTO_STAT(stat_add_sample(core->stat.decode_stall, (int)stall_reason);)
}

void
core_decode_STM_t::recover(struct Mop_t * const Mop)
{
  struct core_knobs_t * knobs = core->knobs;
  /* walk pipe from youngest uop blowing everything away,
     stop if we encounter the recover-Mop */
  for(int stage=0;stage<knobs->decode.depth;stage++)
  {
    if(occupancy[stage])
      for(int i=knobs->decode.width-1;i>=0;i--)
      {
        if(pipe[stage][i])
        {
          if(pipe[stage][i] == Mop)
            return;
          else
          {
            pipe[stage][i] = NULL;
            occupancy[stage]--;
            zesto_assert(occupancy[stage] >= 0,(void)0);
          }
        }
      }
  }
}

/* same as above, but blow away the entire decode pipeline */
void
core_decode_STM_t::recover(void)
{
  struct core_knobs_t * knobs = core->knobs;
  for(int stage=0;stage<knobs->decode.depth;stage++)
  {
    if(occupancy[stage])
      for(int i=knobs->decode.width-1;i>=0;i--)
      {
        if(pipe[stage][i])
        {
          pipe[stage][i] = NULL;
          occupancy[stage]--;
          zesto_assert(occupancy[stage] >= 0,(void)0);
        }
      }
  }
}





bool core_decode_STM_t::uop_available(void)
{
  struct core_knobs_t * knobs = core->knobs;
  int stage = knobs->decode.depth-1;
  return occupancy[stage] != 0; /* if last stage is not empty, a uop is available */
}

struct uop_t * core_decode_STM_t::uop_peek(void)
{
  struct core_knobs_t * knobs = core->knobs;
  struct uop_t * uop = NULL;
  int stage = knobs->decode.depth-1;


  /* assumes uop_available has already been called */
  for(int i=0;i<knobs->decode.width;i++)
  {
    if(pipe[stage][i])
    {
      struct Mop_t * Mop = pipe[stage][i];      /* Mop in current decoder */
      uop = &Mop->uop[Mop->decode.last_stage_index]; /* first non-queued uop */
      
      zesto_assert(uop != NULL,NULL);
      return uop;
    }
  }

  fatal("should have been able to provide a uop, but none were found in decode");

}

void core_decode_STM_t::uop_consume(void)
{
  struct core_knobs_t * knobs = core->knobs;
  struct uop_t * uop = NULL;
  int stage = knobs->decode.depth-1;

  /* assumes uop_available has already been called */
  for(int i=0;i<knobs->decode.width;i++)
  {
    if(pipe[stage][i])
    {
      struct Mop_t * Mop = pipe[stage][i];      /* Mop in current decoder */
      uop = &Mop->uop[Mop->decode.last_stage_index]; /* first non-queued uop */
      Mop->decode.last_stage_index += uop->decode.has_imm ? 3 : 1;  /* increment the uop pointer to next uop */
      
      ZESTO_STAT(core->stat.decode_uops++;)
      uop->timing.when_decoded = core->sim_cycle;
      if(uop->decode.BOM)
        uop->Mop->timing.when_decode_started = core->sim_cycle;
      if(uop->decode.EOM)
        uop->Mop->timing.when_decode_finished = core->sim_cycle;

      if(Mop->decode.last_stage_index >= Mop->decode.flow_length)
      {
        if(Mop->uop[Mop->decode.last_uop_index].decode.EOM)
          ZESTO_STAT(core->stat.decode_insn++;)
        /* all uops dispatched, remove from decoder */
        pipe[stage][i] = NULL;
        occupancy[stage]--;
        zesto_assert(occupancy[stage] >= 0,(void)0);
      }

      zesto_assert(uop != NULL,(void)0);
      return;
    }
  }

  fatal("should have been able to provide a uop, but none were found in decode");

}

#endif
