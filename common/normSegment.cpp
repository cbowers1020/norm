#include "normSegment.h"

#include <errno.h>  // for strerror()

NormSegmentPool::NormSegmentPool()
 : seg_size(0), seg_count(0), seg_total(0), seg_list(NULL),
   peak_usage(0), overruns(0), overrun_flag(false)
{
}


NormSegmentPool::~NormSegmentPool()
{
    Destroy();
}

bool NormSegmentPool::Init(unsigned int count, unsigned int size)
{
    if (seg_list) Destroy();
    peak_usage = 0;
    overruns = 0;        
#ifdef SIMULATE
    // In simulations, don't really need big vectors for data
    // since we don't actually read/write real data (for the most part)
    size = MIN(size, (SIM_PAYLOAD_MAX+1));
#endif  // SIMULATE
    // This makes sure we get appropriate alignment
    unsigned int alloc_size = size / sizeof(char*);
    if ((alloc_size*sizeof(char*)) < size) alloc_size++;
    seg_size = alloc_size * sizeof(char*);
    for (unsigned int i = 0; i < count; i++)
    {
        char** ptr = new char*[alloc_size];
        if (ptr)
        {
            *ptr = seg_list;
            seg_list = (char*)ptr;
            seg_count++;
        }
        else
        {
            DMSG(0, "NormSegmentPool::Init() memory allocation error: %s\n",
                    strerror(errno));
            seg_total = seg_count;
            Destroy();
            return false;
        }
    }  
    seg_total = seg_count;
    return true;
}  // end NormSegmentPool::Init()

void NormSegmentPool::Destroy()
{
    ASSERT(seg_count == seg_total);
    char** ptr = (char**)seg_list;
    while (ptr)
    {
        char* next = *ptr;
        delete ptr;
        ptr = (char**)next;         
    }
    seg_list = NULL;
    seg_count = 0;
    seg_total = 0;
    seg_size = 0;
}  // end NormSegmentPool::Destroy()

char* NormSegmentPool::Get()
{
    char** ptr = (char**)seg_list;
    if (ptr)
    {
        seg_list = *ptr;
        seg_count--;
//#ifdef NORM_DEBUG
        overrun_flag = false;
        unsigned int usage = seg_total - seg_count;
        if (usage > peak_usage) peak_usage = usage;
    }
    else
    {
        if (!overrun_flag)
        {
            DMSG(0, "NormSegmentPool::Get() warning: operating with constrained buffering resources\n");
            overruns++; 
            overrun_flag = true;
        } 
//#endif // NORM_DEBUG 
    }
    return ((char*)ptr);
}  // end NormSegmentPool::GetSegment()


////////////////////////////////////////////////////////////
// NormBlock Implementation

NormBlock::NormBlock()
 : size(0), segment_table(NULL), erasure_count(0), parity_count(0)
{
}     

NormBlock::~NormBlock()
{
    Destroy();
}

bool NormBlock::Init(UINT16 totalSize)
{
    if (segment_table) Destroy();
    if (!(segment_table = new char*[totalSize]))
    {
        DMSG(0, "NormBlock::Init() segment_table allocation error: %s\n", strerror(errno));
        return false;   
    }
    memset(segment_table, 0, totalSize*sizeof(char*));
    if (!pending_mask.Init(totalSize))
    {
        DMSG(0, "NormBlock::Init() pending_mask allocation error: %s\n", strerror(errno));
        Destroy();
        return false;   
    }
    if (!repair_mask.Init(totalSize))
    {
        DMSG(0, "NormBlock::Init() repair_mask allocation error: %s\n", strerror(errno));
        Destroy();
        return false;   
    }
    size = totalSize;
    erasure_count = 0;
    parity_count = 0;
    parity_offset = 0;;
    return true;
}  // end NormBlock::Init()

void NormBlock::Destroy()
{
    repair_mask.Destroy();
    pending_mask.Destroy();
    // (TBD) Option to return segments to pool from which they came
    if (segment_table)
    {
        for (unsigned int i = 0; i < size; i++)
        {
            ASSERT(!segment_table[i]);
            if (segment_table[i]) delete []segment_table[i];
        }
        delete []segment_table;
        segment_table = (char**)NULL;
    }
    erasure_count = parity_count = size = 0;
}  // end NormBlock::Destroy()

void NormBlock::EmptyToPool(NormSegmentPool& segmentPool)
{
    ASSERT(segment_table);
    for (unsigned int i = 0; i < size; i++)
    {
        if (segment_table[i]) 
        {
            segmentPool.Put(segment_table[i]);
            segment_table[i] = (char*)NULL;
        }
    }
}  // end NormBlock::EmptyToPool()

bool NormBlock::IsEmpty() const
{
    ASSERT(segment_table);
    for (unsigned int i = 0; i < size; i++)
        if (segment_table[i]) return false;
    return true;
}  // end NormBlock::EmptyToPool()

// Used by client side to determine if NACK should be sent
// Note: This invalidates the block's "repair_mask" state
bool NormBlock::IsRepairPending(UINT16 numData, UINT16 numParity)
{
    // Clients ask for a block of parity to fulfill their
    // repair needs (erasure_count), but if there isn't 
    // enough parity, they ask for some data segments, too
    if (erasure_count > numParity)
    {
        if (numParity)
        {
            UINT16 i = numParity;
            NormSegmentId nextId = 0;
            GetFirstPending(nextId);
            while (i--)
            {
                // (TBD) for more NACK suppression, we could skip ahead
                // if this bit is already set in repair_mask?
                repair_mask.Set(nextId);  // set bit a parity can fill
                nextId++;
                GetNextPending(nextId);  
            } 
        }
        else if (size > numData)
        {
            repair_mask.SetBits(numData, size-numData);   
        }  
    }
    else
    {
        repair_mask.SetBits(0, numData);
        repair_mask.SetBits(numData+erasure_count, numParity-erasure_count);
    }
    // Calculate repair_mask = pending_mask - repair_mask 
    repair_mask.XCopy(pending_mask);
    return (repair_mask.IsSet());
}  // end NormBlock::IsRepairPending()

// Called by server
bool NormBlock::TxReset(UINT16 numData, 
                        UINT16 numParity, 
                        UINT16 autoParity, 
                        UINT16 segmentSize)
{
    bool increasedRepair = false;
    repair_mask.SetBits(0, numData+autoParity);
    repair_mask.UnsetBits(numData+autoParity, numParity-autoParity);
    repair_mask.Xor(pending_mask);
    if (repair_mask.IsSet()) 
    {
        increasedRepair = true;
        repair_mask.Clear();
        pending_mask.SetBits(0, numData+autoParity);
        pending_mask.UnsetBits(numData+autoParity, numParity-autoParity);
        parity_offset = autoParity;  // reset parity since we're resending this one
        parity_count = numParity;      // no parity repair this repair cycle
        SetFlag(IN_REPAIR);
        if (!ParityReady(numData))  // (TBD) only when incrementalParity == true
        {
            // Clear _any_ existing incremental parity state
            char** ptr = segment_table+numData;
            while (numParity--)
            {
                if (*ptr) 
                {
                    UINT16 payloadMax = segmentSize + 
                                        NormDataMsg::GetStreamPayloadHeaderLength();
#ifdef SIMULATE
                    payloadMax = MIN(payloadMax, SIM_PAYLOAD_MAX);
#endif // SIMULATE
                    memset(*ptr, 0, payloadMax+1);
                }
                ptr++;
            }
            erasure_count = 0;
        }
    }
    return increasedRepair;
}  // end NormBlock::TxReset()

bool NormBlock::ActivateRepairs(UINT16 numParity)
{
    if (repair_mask.IsSet())
    {
        pending_mask.Add(repair_mask);
        repair_mask.Clear();   
        return true;
    }
    else
    {
        return false;
    }
}  // end NormBlock::ActivateRepairs()

// For NACKs arriving during server repair_timer "holdoff" time
// (we directly update the "pending_mask" for blocks/segments
//  greater than our current transmit index)
bool NormBlock::TxUpdate(NormSegmentId nextId, NormSegmentId lastId,
                         UINT16 numData, UINT16 numParity, UINT16 erasureCount)
{
    bool increasedRepair = false;
        
    if (nextId < numData)
    {
        // Explicit data repair request
        parity_offset = parity_count = numParity;
        while (nextId <= lastId)
        {
            if (!pending_mask.Test(nextId))
            {
                pending_mask.Set(nextId);
                increasedRepair = true;
            }
            nextId++;      
        }
    }
    else
    {
        // parity repair request
        UINT16 parityAvailable = numParity - parity_offset;
        if (erasureCount <= parityAvailable)
        {
           // Use fresh parity for repair
           if (erasureCount > parity_count)
           {
               pending_mask.SetBits(numData+parity_offset+parity_count, 
                                    erasureCount - parity_count);
               parity_count = erasureCount;
               increasedRepair = true; 
           }
        }
        else
        {
            // Use any remaining fresh parity ...
            if (parity_count < parityAvailable)
            {
                UINT16 count = parityAvailable - parity_count;
                pending_mask.SetBits(numData+parity_offset+parity_count, count); 
                parity_count = parityAvailable;  
                nextId += parityAvailable;
                increasedRepair = true;
            }
            // and explicit repair for the rest
            while (nextId <= lastId)
            {
                if (!pending_mask.Test(nextId))
                {
                    pending_mask.Set(nextId);
                    increasedRepair = true;
                }
                nextId++; 
            } 
        }   
    }
    return increasedRepair;
}  // end NormBlock::TxUpdate()

bool NormBlock::HandleSegmentRequest(NormSegmentId nextId, NormSegmentId lastId,
                                     UINT16 numData, UINT16 numParity, UINT16 erasureCount)
{
    DMSG(6, "NormBlock::HandleSegmentRequest() blk>%lu seg>%hu:%hu erasures:%hu\n",
            (UINT32)id, (UINT16)nextId, (UINT16)lastId, erasureCount);
    bool increasedRepair = false;
    if (nextId < numData)
    {
        // Explicit data repair request
        parity_count = parity_offset = numParity;
        while (nextId <= lastId)
        {
            if (!repair_mask.Test(nextId))
            {
                repair_mask.Set(nextId);
                increasedRepair = true;
            }
            nextId++; 
        }   
    }
    else
    {
        // parity repair request
        UINT16 parityAvailable = numParity - parity_offset;
        if (erasureCount <= parityAvailable)
        {
           // Use fresh parity for repair
           if (erasureCount > parity_count)
           {
               repair_mask.SetBits(numData+parity_offset+parity_count, 
                                   erasureCount - parity_count);
               parity_count = erasureCount;
               increasedRepair = true; 
           }
        }
        else
        {
            // Use any remaining fresh parity ...
            if (parity_count < parityAvailable)
            {
                UINT16 count = parityAvailable - parity_count;
                repair_mask.SetBits(numData+parity_offset+parity_count, count); 
                parity_count = parityAvailable;  
                nextId += parityAvailable;
                increasedRepair = true;
            }
            // and explicit repair for the rest
            while (nextId <= lastId)
            {
                if (!repair_mask.Test(nextId))
                {
                    repair_mask.Set(nextId);
                    increasedRepair = true;
                }
                nextId++; 
            } 
        }   
    }
    return increasedRepair;
}  // end NormBlock::HandleSegmentRequest()


bool NormBlock::AppendRepairAdv(NormCmdRepairAdvMsg& cmd, 
                                NormObjectId         objectId,
                                bool                 repairInfo,
                                UINT16               numData,
                                UINT16               segmentSize)
{
    NormRepairRequest req;
    req.SetFlag(NormRepairRequest::SEGMENT);
    if (repairInfo) req.SetFlag(NormRepairRequest::INFO);
    NormSymbolId nextId = 0;
    if (GetFirstRepair(nextId))
    {
        UINT16 totalSize = size;
        NormRepairRequest::Form prevForm = NormRepairRequest::INVALID;
        UINT16 segmentCount = 0;
        UINT16 firstId = 0;
        while (nextId < totalSize)
        {
            UINT16 currentId = nextId;
            if (!GetNextRepair(++nextId)) nextId = totalSize;
            if (!segmentCount) firstId = currentId;
            segmentCount++;
            // Check for break in consecutive series or end
            if (((nextId  - currentId) > 1) || (nextId >= totalSize))
            {
                NormRepairRequest::Form form;
                switch (segmentCount)
                {
                    case 0:
                        form = NormRepairRequest::INVALID;
                        break;
                    case 1:
                    case 2:
                        form = NormRepairRequest::ITEMS;
                        break;
                    default:
                        form = NormRepairRequest::RANGES;
                        break;
                }
                if (form != prevForm)
                {
                    if (NormRepairRequest::INVALID != prevForm) 
                        cmd.PackRepairRequest(req);            // (TBD) error check
                    req.SetForm(form);
                    cmd.AttachRepairRequest(req, segmentSize); // (TBD) error check
                    prevForm = form;
                }            
                switch(form)
                {
                    case NormRepairRequest::INVALID:
                        ASSERT(0);  // can't happen
                        break;
                    case NormRepairRequest::ITEMS:
                        req.AppendRepairItem(objectId, id, numData, firstId);
                        if (2 == segmentCount) 
                            req.AppendRepairItem(objectId, id, numData, currentId);
                        break;
                    case NormRepairRequest::RANGES:
                        req.AppendRepairRange(objectId, id, numData, firstId,
                                              objectId, id, numData, currentId);
                        break;
                    case NormRepairRequest::ERASURES:
                        // erasure counts not used
                        break;
                } 
                segmentCount = 0;  
            }
        }  // end while (nextId < totalSize)
        if (NormRepairRequest::INVALID != prevForm) 
            cmd.PackRepairRequest(req); // (TBD) error check
    }
    return true;
}  // end NormBlock::AppendRepairAdv()

// Called by client
bool NormBlock::AppendRepairRequest(NormNackMsg&    nack, 
                                    UINT16          numData, 
                                    UINT16          numParity,
                                    NormObjectId    objectId,
                                    bool            pendingInfo,
                                    UINT16          segmentSize)
{
    NormSegmentId nextId = 0;
    NormSegmentId endId;
    if (erasure_count > numParity)
    {
        // Request explicit repair 
        GetFirstPending(nextId);
        UINT16 i = numParity;
        // Skip numParity missing data segments
        while (i--)
        {
            nextId++;
            GetNextPending(nextId);
        }
        endId = numData + numParity;
    }
    else
    {
        nextId = numData;
        GetNextPending(nextId);
        endId = numData + erasure_count;   
    }
    NormRepairRequest req;
    req.SetFlag(NormRepairRequest::SEGMENT);                  
    if (pendingInfo) req.SetFlag(NormRepairRequest::INFO);  
    NormRepairRequest::Form prevForm = NormRepairRequest::INVALID;
    UINT16 segmentCount = 0;
    // new code begins here  
    UINT16 firstId = 0;
    while (nextId < endId)
    {
        UINT16 currentId = nextId;
        if (!GetNextPending(++nextId)) nextId = endId;
        if (0 == segmentCount) firstId = currentId;
        segmentCount++;
        // Check for break in consecutive series or end
        if (((nextId  - currentId) > 1) || (nextId >= endId))
        {
            NormRepairRequest::Form form;
            switch (segmentCount)
            {
                case 0:
                    form = NormRepairRequest::INVALID;
                    break;
                case 1:
                case 2:
                    form = NormRepairRequest::ITEMS;
                    break;
                default:
                    form = NormRepairRequest::RANGES;
                    break;
            }   
            if (form != prevForm)
            {
                if (NormRepairRequest::INVALID != prevForm) 
                    nack.PackRepairRequest(req);                       // (TBD) error check
                nack.AttachRepairRequest(req, segmentSize);            // (TBD) error check
                req.SetForm(form);
                prevForm = form;
            }
            switch (form)
            {
                case NormRepairRequest::INVALID:
                    ASSERT(0);
                    break;
                case NormRepairRequest::ITEMS:
                    req.AppendRepairItem(objectId, id, numData, firstId);       // (TBD) error check
                    if (2 == segmentCount)
                        req.AppendRepairItem(objectId, id, numData, currentId); // (TBD) error check
                    break;
                case NormRepairRequest::RANGES:
                    req.AppendRepairRange(objectId, id, numData, firstId,       // (TBD) error check
                                          objectId, id, numData, currentId);    // (TBD) error check
                    break;
                case NormRepairRequest::ERASURES:
                    // erasure counts not used
                    break;
            }  // end switch(form)
            segmentCount = 0;
        }       
    }  // end while (nextId < lastId)
    if (NormRepairRequest::INVALID != prevForm) 
        nack.PackRepairRequest(req);                                   // (TBD) error check
    
    /*
    // old code begins here
    NormSegmentId prevId = nextId;
    while ((nextId <= lastId) || (segmentCount > 0))
    {
        // force break of possible ending consec. series
        if (nextId == lastId) nextId++; 
        if (segmentCount && (segmentCount == (nextId - prevId)))
        {
            segmentCount++;  // consecutive series continues
        }
        else
        {
            NormRepairRequest::Form nextForm;
            switch(segmentCount)
            {
                case 0:
                    nextForm = NormRepairRequest::INVALID;
                    break;
                case 1:
                case 2:
                    nextForm = NormRepairRequest::ITEMS;
                    break;
                default:
                    nextForm = NormRepairRequest::RANGES;
                    break;
            }  // end switch(reqCount)
            if (prevForm != nextForm)
            {
                if (NormRepairRequest::INVALID != prevForm)
                    nack.PackRepairRequest(req);  // (TBD) error check
                if (NormRepairRequest::INVALID != nextForm)
                {
                    nack.AttachRepairRequest(req, segmentSize);
                    req.SetForm(nextForm);
                    req.SetFlag(NormRepairRequest::SEGMENT);
                    if (pendingInfo) req.SetFlag(NormRepairRequest::INFO);
                }
                prevForm = nextForm;
            }
            if (NormRepairRequest::INVALID != nextForm)
                DMSG(6, "NormBlock::AppendRepairRequest() SEGMENT request\n");
            switch (nextForm)
            {
                case NormRepairRequest::ITEMS:
                    req.AppendRepairItem(objectId, id, prevId);  // (TBD) error check
                    if (2 == reqCount)
                        req.AppendRepairItem(objectId, id, prevId+1); // (TBD) error check
                    break;
                case NormRepairRequest::RANGES:
                    req.AppendRepairItem(objectId, id, prevId); // (TBD) error check
                    req.AppendRepairItem(objectId, id, prevId+reqCount-1); // (TBD) error check
                    break;
                default:
                    break;
            }  // end switch(nextForm)
            prevId = nextId;
            if (nextId < lastId)
                segmentCount = 1;
            else
                segmentCount = 0;
        }  // end if/else (reqCount && (reqCount == (nextId - prevId)))
        nextId++;
        if (nextId <= lastId) nextId = pending_mask.NextSet(nextId);
    }  // end while(nextId <= lastId)
    if (NormRepairRequest::INVALID != prevForm)
        nack.PackRepairRequest(req);  // (TBD) error check
    */
    return true;
}  // end NormBlock::AppendRepairRequest()
         
NormBlockPool::NormBlockPool()
 : head((NormBlock*)NULL), overruns(0), overrun_flag(false)
{
}

NormBlockPool::~NormBlockPool()
{
    Destroy();
}

bool NormBlockPool::Init(UINT32 numBlocks, UINT16 totalSize)
{
    if (head) Destroy();
    for (UINT32 i = 0; i < numBlocks; i++)
    {
        NormBlock* b = new NormBlock();
        if (b)
        {
            if (!b->Init(totalSize))
            {
                DMSG(0, "NormBlockPool::Init() block init error\n");
                delete b;
                Destroy();
                return false;   
            }  
            b->next = head;
            head = b;
        }
        else
        {
            DMSG(0, "NormBlockPool::Init() new block error\n");
            Destroy();
            return false; 
        } 
    }
    return true;
}  // end NormBlockPool::Init()

void NormBlockPool::Destroy()
{
    NormBlock* next;
    while ((next = head))
    {
        head = next->next;
        delete next;   
    }
}  // end NormBlockPool::Destroy()

NormBlockBuffer::NormBlockBuffer()
 : table((NormBlock**)NULL), range_max(0), range(0)
{
}

NormBlockBuffer::~NormBlockBuffer()
{
    Destroy();
}

bool NormBlockBuffer::Init(unsigned long rangeMax, unsigned long tableSize)
{
    if (table) Destroy();
    // Make sure tableSize is greater than 0 and 2^n
    if (!rangeMax || !tableSize) 
    {
        DMSG(0, "NormBlockBuffer::Init() bad range(%lu) or tableSize(%lu)\n",
                rangeMax, tableSize);
        return false;
    }
    if (0 != (tableSize & 0x07)) tableSize = (tableSize >> 3) + 1;
    if (!(table = new NormBlock*[tableSize]))
    {
        DMSG(0, "NormBlockBuffer::Init() buffer allocation error: %s\n", strerror(errno));
        return false;         
    }
    memset(table, 0, tableSize*sizeof(char*));
    hash_mask = tableSize - 1;
    range_max = rangeMax;
    range = 0;
    return true;
}  // end NormBlockBuffer::Init()

void NormBlockBuffer::Destroy()
{
    range_max = range = 0;  
    if (table)
    {
        NormBlock* block;
        while((block = Find(range_lo)))
        {
            DMSG(0, "NormBlockBuffer::Destroy() buffer not empty!?\n");
            Remove(block);
            delete block;   
        }
        delete []table;
        table = (NormBlock**)NULL;
        range_max = 0;
    }     
}  // end NormBlockBuffer::Destroy()

NormBlock* NormBlockBuffer::Find(const NormBlockId& blockId) const
{
    if (range)
    {
        if ((blockId < range_lo)  || (blockId > range_hi)) 
            return (NormBlock*)NULL;
        NormBlock* theBlock = table[((UINT32)blockId) & hash_mask];
        while (theBlock && (blockId != theBlock->GetId())) 
            theBlock = theBlock->next;
        return theBlock;
    }
    else
    {
        return (NormBlock*)NULL;
    }   
}  // end NormBlockBuffer::Find()


bool NormBlockBuffer::CanInsert(NormBlockId blockId) const
{
    if (0 != range)
    {
        if (blockId < range_lo)
        {
            if ((range_lo - blockId + range) > range_max)
                return false;
            else
                return true;
        }
        else if (blockId > range_hi)
        {
            if ((blockId - range_hi + range) > range_max)
                return false;
            else
                return true;
        }
        else
        {
            return true;
        }        
    }
    else
    {
        return true;
    }    
}  // end NormBlockBuffer::CanInsert()


bool NormBlockBuffer::Insert(NormBlock* theBlock)
{
    const NormBlockId& blockId = theBlock->GetId();
    if (!range)
    {
        range_lo = range_hi = blockId;
        range = 1;   
    }
    if (blockId < range_lo)
    {
        UINT32 newRange = range_lo - blockId + range;
        if (newRange > range_max) return false;
        range_lo = blockId;
        range = newRange;
    }
    else if (blockId > range_hi)
    {            
        UINT32 newRange = blockId - range_hi + range;
        if (newRange > range_max) return false;
        range_hi = blockId;
        range = newRange;
    }
    UINT32 index = ((UINT32)blockId) & hash_mask;
    NormBlock* prev = NULL;
    NormBlock* entry = table[index];
    while (entry && (entry->GetId() < blockId)) 
    {
        prev = entry;
        entry = entry->next;
    }  
    if (prev)
        prev->next = theBlock;
    else
        table[index] = theBlock;
    
    ASSERT((entry ? (blockId != entry->GetId()) : true));
    
    theBlock->next = entry;
    return true;
}  // end NormBlockBuffer::Insert()

bool NormBlockBuffer::Remove(const NormBlock* theBlock)
{
    ASSERT(theBlock);
    if (range)
    {
        const NormBlockId& blockId = theBlock->GetId();
        if ((blockId < range_lo) || (blockId > range_hi)) return false;
        UINT32 index = ((UINT32)blockId) & hash_mask;
        NormBlock* prev = NULL;
        NormBlock* entry = table[index];
        while (entry && (entry->GetId() != blockId))
        {
            prev = entry;
            entry = entry->next;
        }
        if (!entry) return false;
        if (prev)
            prev->next = entry->next;
        else
            table[index] = entry->next;
        
        if (range > 1)
        {
            if (blockId == range_lo)
            {
                // Find next entry for range_lo
                UINT32 i = index;
                UINT32 endex;
                if (range <= hash_mask)
                    endex = (index + range - 1) & hash_mask;
                else
                    endex = index;
                entry = NULL;
                UINT32 offset = 0;
                NormBlockId nextId = range_hi;
                do
                {
                    ++i &= hash_mask;
                    offset++;
                    if ((entry = table[i]))
                    {
                        //NormBlockId id = (UINT32)index + offset;
                        NormBlockId id = (UINT32)blockId + offset;
                        while(entry && (entry->GetId() != id)) 
                        {
                            if ((entry->GetId() > blockId) && 
                                (entry->GetId() < nextId)) nextId = entry->GetId();
                            entry = entry->next;
                               
                        }
                        if (entry) break;    
                    }
                } while (i != endex);
                if (entry)
                    range_lo = entry->GetId();
                else
                    range_lo = nextId;
                range = range_hi - range_lo + 1; 
            }
            else if (blockId == range_hi)
            {
                // Find prev entry for range_hi
                UINT32 i = index;
                UINT32 endex;
                if (range <= hash_mask)
                    endex = (index - range + 1) & hash_mask;
                else
                    endex = index;
                entry = NULL;
                UINT32 offset = 0;
                //printf("preving i:%lu endex:%lu lo:%lu hi:%lu\n", i, endex, (UINT32)range_lo, (UINT32) range_hi);
                NormBlockId prevId = range_lo;
                do
                {
                    --i &= hash_mask;
                    offset++;
                    if ((entry = table[i]))
                    {
                        //NormBlockId id = (UINT32)index - offset;
                        NormBlockId id = (UINT32)blockId - offset;
                        //printf("Looking for id:%lu at index:%lu\n", (UINT32)id, i);
                        while(entry && (entry->GetId() != id)) 
                        {
                            if ((entry->GetId() < blockId) && 
                                (entry->GetId() > prevId)) prevId = entry->GetId();
                            entry = entry->next;
                        }
                        if (entry) break;    
                    }
                } while (i != endex);
                if (entry)
                    range_hi = entry->GetId();
                else 
                    range_hi = prevId;
                range = range_hi - range_lo + 1;
            } 
        }
        else
        {
            range = 0;
        }  
        return true;
    }
    else
    {
        return false;
    }
}  // end NormBlockBuffer::Remove()

NormBlockBuffer::Iterator::Iterator(const NormBlockBuffer& blockBuffer)
 : buffer(blockBuffer), reset(true)
{
}

NormBlock* NormBlockBuffer::Iterator::GetNextBlock()
{
    if (reset)
    {
        if (buffer.range)
        {
            reset = false;
            index = buffer.range_lo;
            return buffer.Find(index);
        }
        else
        {
            return (NormBlock*)NULL;
        }
    }
    else
    {
        if (buffer.range && 
            (index < buffer.range_hi) && 
            (index >= buffer.range_lo))
        {
            // Find next entry _after_ current "index"
            UINT32 i = index;
            UINT32 endex;
            if ((UINT32)(buffer.range_hi - index) <= buffer.hash_mask)
                endex = buffer.range_hi & buffer.hash_mask;
            else
                endex = index;
            UINT32 offset = 0;
            NormBlockId nextId = buffer.range_hi;
            do
            {
                ++i &= buffer.hash_mask;
                offset++;
                NormBlockId id = (UINT32)index + offset;
                ASSERT(i < 256);
                NormBlock* entry = buffer.table[i];
                while ((NULL != entry ) && (entry->GetId() != id)) 
                {
                    if ((entry->GetId() > index) && (entry->GetId() < nextId))
                        nextId = entry->GetId();
                    entry = NormBlockBuffer::Next(entry);
                }
                if (entry)
                {
                    index = entry->GetId();
                    return entry;   
                } 
            } while (i != endex);
            // If we get here, use nextId value
            index = nextId;
            return buffer.Find(nextId);
        }
        else
        {
            return (NormBlock*)NULL;
        }
    }   
}  // end NormBlockBuffer::Iterator::GetNextBlock()
