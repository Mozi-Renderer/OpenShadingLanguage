/*
Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/strutil.h>

#include "oslexec_pvt.h"
#include "backendllvm_wide.h"

#include <llvm/IR/Type.h>

using namespace OSL;
using namespace OSL::pvt;

OSL_NAMESPACE_ENTER

namespace pvt {

static ustring op_if("if");
static ustring op_for("for");
static ustring op_dowhile("dowhile");
static ustring op_while("while");
static ustring op_functioncall("functioncall");
static ustring op_break("break");
static ustring op_continue("continue");
static ustring op_getattribute("getattribute");



#ifdef OSL_SPI
static void
check_cwd (ShadingSystemImpl &shadingsys)
{
    std::string err;
    char pathname[1024] = { "" };
    if (! getcwd (pathname, sizeof(pathname)-1)) {
        int e = errno;
        err += Strutil::format ("Failed getcwd(), errno is %d: %s\n",
                                errno, pathname);
        if (e == EACCES || e == ENOENT) {
            err += "Read/search permission problem or dir does not exist.\n";
            const char *pwdenv = getenv ("PWD");
            if (! pwdenv) {
                err += "$PWD is not even found in the environment.\n";
            } else {
                err += Strutil::format ("$PWD is \"%s\"\n", pwdenv);
                err += Strutil::format ("That %s.\n",
                          OIIO::Filesystem::exists(pwdenv) ? "exists" : "does NOT exist");
                err += Strutil::format ("That %s a directory.\n",
                          OIIO::Filesystem::is_directory(pwdenv) ? "is" : "is NOT");
                std::vector<std::string> pieces;
                Strutil::split (pwdenv, pieces, "/");
                std::string p;
                for (size_t i = 0;  i < pieces.size();  ++i) {
                    if (! pieces[i].size())
                        continue;
                    p += "/";
                    p += pieces[i];
                    err += Strutil::format ("  %s : %s and is%s a directory.\n", p,
                        OIIO::Filesystem::exists(p) ? "exists" : "does NOT exist",
                        OIIO::Filesystem::is_directory(p) ? "" : " NOT");
                }
            }
        }
    }
    if (err.size())
        shadingsys.error (err);
}
#endif



BackendLLVMWide::BackendLLVMWide (ShadingSystemImpl &shadingsys,
                          ShaderGroup &group, ShadingContext *ctx)
    : OSOProcessorBase (shadingsys, group, ctx),
      ll(llvm_debug()),
      m_stat_total_llvm_time(0), m_stat_llvm_setup_time(0),
      m_stat_llvm_irgen_time(0), m_stat_llvm_opt_time(0),
      m_stat_llvm_jit_time(0)
{
#ifdef OSL_SPI
    // Temporary (I hope) check to diagnose an intermittent failure of
    // getcwd inside LLVM. Oy.
    check_cwd (shadingsys);
#endif
}



BackendLLVMWide::~BackendLLVMWide ()
{
}



int
BackendLLVMWide::llvm_debug() const
{
    if (shadingsys().llvm_debug() == 0)
        return 0;
    if (shadingsys().debug_groupname() &&
        shadingsys().debug_groupname() != group().name())
        return 0;
    if (inst() && shadingsys().debug_layername() &&
        shadingsys().debug_layername() != inst()->layername())
        return 0;
    return shadingsys().llvm_debug();
}



void
BackendLLVMWide::set_inst (int layer)
{
    OSOProcessorBase::set_inst (layer);  // parent does the heavy lifting
    ll.debug (llvm_debug());
}



llvm::Type *
BackendLLVMWide::llvm_pass_type (const TypeSpec &typespec)
{
    if (typespec.is_closure_based())
        return (llvm::Type *) ll.type_void_ptr();
    TypeDesc t = typespec.simpletype().elementtype();
    llvm::Type *lt = NULL;
    if (t == TypeDesc::FLOAT)
        lt = ll.type_float();
    else if (t == TypeDesc::INT)
        lt = ll.type_int();
    else if (t == TypeDesc::STRING)
        lt = (llvm::Type *) ll.type_string();
    else if (t.aggregate == TypeDesc::VEC3)
        lt = (llvm::Type *) ll.type_void_ptr(); //llvm_type_triple_ptr();
    else if (t.aggregate == TypeDesc::MATRIX44)
        lt = (llvm::Type *) ll.type_void_ptr(); //llvm_type_matrix_ptr();
    else if (t == TypeDesc::NONE)
        lt = ll.type_void();
    else if (t == TypeDesc::PTR)
        lt = (llvm::Type *) ll.type_void_ptr();
    else if (t == TypeDesc::LONGLONG)
        lt = ll.type_longlong();
    else {
        std::cerr << "Bad llvm_pass_type(" << typespec.c_str() << ")\n";
        ASSERT (0 && "not handling this type yet");
    }
    if (t.arraylen) {
        ASSERT (0 && "should never pass an array directly as a parameter");
    }
    return lt;
}

llvm::Type *
BackendLLVMWide::llvm_pass_wide_type (const TypeSpec &typespec)
{
    if (typespec.is_closure_based())
        return (llvm::Type *) ll.type_void_ptr();
    TypeDesc t = typespec.simpletype().elementtype();
    llvm::Type *lt = NULL;
    if (t == TypeDesc::FLOAT)
        lt = (llvm::Type *)ll.type_void_ptr(); // ll.type_wide_float();
    else if (t == TypeDesc::INT)
        lt = (llvm::Type *)ll.type_void_ptr(); // ll.type_wide_int();
    else if (t == TypeDesc::STRING)
        lt = (llvm::Type *)ll.type_void_ptr(); // (llvm::Type *) ll.type_wide_ string();
    else if (t.aggregate == TypeDesc::VEC3)
        lt = (llvm::Type *) ll.type_void_ptr(); //llvm_type_wide_triple_ptr();
    else if (t.aggregate == TypeDesc::MATRIX44)
        lt = (llvm::Type *) ll.type_void_ptr(); //llvm_type_wide_matrix_ptr();
    else if (t == TypeDesc::NONE)
        lt = ll.type_void();
    else if (t == TypeDesc::PTR)
        lt = (llvm::Type *) ll.type_void_ptr();
    else if (t == TypeDesc::LONGLONG)
        lt = (llvm::Type *)ll.type_void_ptr(); // ll.type_wide_longlong();
    else {
        std::cerr << "Bad llvm_pass_type(" << typespec.c_str() << ")\n";
        ASSERT (0 && "not handling this type yet");
    }
    if (t.arraylen) {
        ASSERT (0 && "should never pass an array directly as a parameter");
    }
    return lt;
}



void
BackendLLVMWide::llvm_assign_zero (const Symbol &sym)
{
    // Just memset the whole thing to zero, let LLVM sort it out.
    // This even works for closures.
    int len;
    if (sym.typespec().is_closure_based())
        len = sizeof(void *) * sym.typespec().numelements();
    else
        len = sym.derivsize();
    // N.B. derivsize() includes derivs, if there are any
    size_t align = sym.typespec().is_closure_based() ? sizeof(void*) :
                         sym.typespec().simpletype().basesize();
    ll.op_memset (llvm_void_ptr(sym), 0, len, (int)align);
}



void
BackendLLVMWide::llvm_zero_derivs (const Symbol &sym)
{
    if (sym.typespec().is_closure_based())
        return; // Closures don't have derivs
    // Just memset the derivs to zero, let LLVM sort it out.
    TypeSpec elemtype = sym.typespec().elementtype();
    if (sym.has_derivs() && elemtype.is_floatbased()) {
        int len = sym.size();
        size_t align = sym.typespec().simpletype().basesize();
        ll.op_memset (llvm_void_ptr(sym,1), /* point to start of x deriv */
                      0, 2*len /* size of both derivs */, (int)align);
    }
}



void
BackendLLVMWide::llvm_zero_derivs (const Symbol &sym, llvm::Value *count)
{
    if (sym.typespec().is_closure_based())
        return; // Closures don't have derivs
    // Same thing as the above version but with just the first count derivs
    TypeSpec elemtype = sym.typespec().elementtype();
    if (sym.has_derivs() && elemtype.is_floatbased()) {
        size_t esize = sym.typespec().simpletype().elementsize();
        size_t align = sym.typespec().simpletype().basesize();
        count = ll.op_mul (count, ll.constant((int)esize));
        ll.op_memset (llvm_void_ptr(sym,1), 0, count, (int)align); // X derivs
        ll.op_memset (llvm_void_ptr(sym,2), 0, count, (int)align); // Y derivs
    }
}

namespace
{
    // N.B. The order of names in this table MUST exactly match the
    // ShaderGlobalsBatch struct in ShaderGlobals.h, as well as the llvm 'sg' type
    // defined in llvm_type_sg().
    static ustring fields[] = {
		// Uniform
		ustring("renderstate"),
		ustring("tracedata"), 
		ustring("objdata"),
		ustring("shadingcontext"), 
        ustring("renderer"),
        ustring("Ci"),
		ustring("raytype"),
		ustring("pad0"),
		ustring("pad1"),
		ustring("pad2"),
		// Varying
        ustring("P"), 
		ustring("dPdz"), 
		ustring("I"),
        ustring("N"), 
		ustring("Ng"),
        ustring("u"), 
		ustring("v"), 
		ustring("dPdu"), 
		ustring("dPdv"),
        ustring("time"), 
		ustring("dtime"), 
		ustring("dPdtime"), 
		ustring("Ps"),        
        ustring("object2common"), 
		ustring("shader2common"),
        ustring("surfacearea"), 
        ustring("flipHandedness"), 
		ustring("backfacing")
    };

    static bool field_is_uniform[] = {
		// Uniform
		true, //ustring("renderstate"),
		true, //ustring("tracedata"), 
		true, //ustring("objdata"),
		true, //ustring("shadingcontext"), 
		true, //ustring("renderer"),
		true, //ustring("Ci"),
		true, //ustring("raytype"),
		true, //ustring("pad0"),
		true, //ustring("pad1"),
		true, //ustring("pad2"),
		// Varying
        false, //ustring("P"), 
		false, //ustring("dPdz"), 
		false, //ustring("I"),
		false, //ustring("N"), 
		false, //ustring("Ng"),
		false, //ustring("u"), 
		false, //ustring("v"), 
		false, //ustring("dPdu"), 
		false, //ustring("dPdv"),
		false, //ustring("time"), 
		false, //ustring("dtime"), 
		false, //ustring("dPdtime"), 
		false, //ustring("Ps"),        
		false, //ustring("object2common"), 
		false, //ustring("shader2common"),
		false, //ustring("surfacearea"), 
		false, //ustring("flipHandedness"), 
		false, //ustring("backfacing")
    };
    
    
    bool
    IsShaderGlobalUniformByName (ustring name)
    {
        for (int i = 0;  i < int(sizeof(fields)/sizeof(fields[0]));  ++i) {
            if (name == fields[i]) {
            	return field_is_uniform[i];
            }
        }
        return false;
    }
    
}

int
BackendLLVMWide::ShaderGlobalNameToIndex (ustring name, bool &is_uniform)
{
    for (int i = 0;  i < int(sizeof(fields)/sizeof(fields[0]));  ++i)
        if (name == fields[i]) {
        	is_uniform = field_is_uniform[i];
            return i;
        }
    std::cout << "ShaderGlobalNameToIndex failed with " << name << std::endl;
    return -1;
}



llvm::Value *
BackendLLVMWide::llvm_global_symbol_ptr (ustring name, bool &is_uniform)
{
    // Special case for globals -- they live in the ShaderGlobals struct,
    // we use the name of the global to find the index of the field within
    // the ShaderGlobals struct.
    int sg_index = ShaderGlobalNameToIndex (name, is_uniform);
    ASSERT (sg_index >= 0);
    return ll.void_ptr (ll.GEP (sg_ptr(), 0, sg_index));
}

llvm::Value *
BackendLLVMWide::getLLVMSymbolBase (const Symbol &sym)
{
    Symbol* dealiased = sym.dealias();

	bool is_uniform = isSymbolUniform(sym);
	
    if (sym.symtype() == SymTypeGlobal) {
        llvm::Value *result = llvm_global_symbol_ptr (sym.name(), is_uniform);
        ASSERT (result);
        if (is_uniform) {
        	result = ll.ptr_to_cast (result, llvm_type(sym.typespec().elementtype()));
        } else {
        	result = ll.ptr_to_cast (result, llvm_wide_type(sym.typespec().elementtype()));
        }
        return result;
    }

    if (sym.symtype() == SymTypeParam || sym.symtype() == SymTypeOutputParam) {
        // Special case for params -- they live in the group data
        int fieldnum = m_param_order_map[&sym];
        return groupdata_field_ptr (fieldnum, sym.typespec().elementtype().simpletype(), is_uniform);
    }

    std::string mangled_name = dealiased->mangled();
    AllocationMap::iterator map_iter = named_values().find (mangled_name);
    if (map_iter == named_values().end()) {
        shadingcontext()->error ("Couldn't find symbol '%s' (unmangled = '%s'). Did you forget to allocate it?",
                            mangled_name.c_str(), dealiased->name().c_str());
        return 0;
    }
    return (llvm::Value*) map_iter->second;
}


void 
BackendLLVMWide::discoverVaryingAndMaskingOfLayer()
{
	std::cout << "start discoverVaryingAndMaskingOfLayer of layer=" << layer() << std::endl;
	
	const OpcodeVec & opcodes = inst()->ops();
	
	ASSERT(m_requires_masking_by_layer_and_op_index.size() > layer());	
	ASSERT(m_requires_masking_by_layer_and_op_index[layer()].empty());
	m_requires_masking_by_layer_and_op_index[layer()].resize(opcodes.size(), false);
			
	// TODO:  Optimize: could probably use symbol index vs. a pointer 
	// allowing a lookup table vs. hash_map
	 
	
	std::unordered_multimap<const Symbol * /* parent */ , const Symbol * /* dependent */> symbolFeedForwardMap;

	struct UsageInfo
	{
		int last_depth;
		int last_maskId;
		std::vector <std::pair<int /*blockDepth*/, int /*op_num*/>> potentially_unmasked_ops;
		
		UsageInfo()
		:last_depth(0)
		,last_maskId(0)
		{}				
	};
	std::unordered_map<const Symbol *, UsageInfo > usageInfoBySymbol;
	
	std::vector<const Symbol *> symbolsCurrentBlockDependsOn;
	std::vector<const Symbol *> loopControlFlowSymbolStack;

    std::vector<const Symbol *> symbolsWrittenToByGetAttribute;

	
	std::function<void(const Symbol *, int, int)> ensureWritesAtLowerDepthAreMasked;
	ensureWritesAtLowerDepthAreMasked = [&](const Symbol *symbolToCheck, int blockDepth, int maskId)->void {			    			
		// Check if reading a Symbol that was written to from a different 
		// maskId than we are reading, if so we need to mark it as requiring masking
		auto lookup = usageInfoBySymbol.find(symbolToCheck);
		if(lookup != usageInfoBySymbol.end()) {
			UsageInfo & info = lookup->second;
			if ((info.last_depth > blockDepth) && (info.last_maskId != maskId))
			{
				std::cout << symbolToCheck->name() << " will need to have last write be masked" << std::endl;
				ASSERT(info.potentially_unmasked_ops.empty() == false);
				decltype(info.potentially_unmasked_ops) remaining_ops;
				for(auto usage: info.potentially_unmasked_ops) {
					// Only mark deeper usages as requiring masking
					if(usage.first > blockDepth)
					{
						std::cout << " marking op " << usage.second << " as masked" << std::endl;
						m_requires_masking_by_layer_and_op_index[layer()][usage.second] = true;									
					} else {
						remaining_ops.push_back(usage);
					}
				}
				
				info.potentially_unmasked_ops.swap(remaining_ops);		
				// Now that all ops writing to the symbol at higher depths have been marked to be masked
				// we can now consider the matter handled at this point at reset the
				// last_depth written at to the current depth to avoid needlessly repeating the work.
				info.last_depth = blockDepth;
			}
		}
	};
    
	int nextMaskId = 0;
	int blockId = 0;
	std::function<void(int, int, int, int, int, int)> discoverSymbolsBetween;
	discoverSymbolsBetween = [&](int beginop, int endop, int blockDepth, int writeBlockDepth, int maskId, int writeMaskId)->void
	{		
		std::cout << "discoverSymbolsBetween [" << beginop << "-" << endop <<"]" << std::endl;
		// NOTE: allowing a seperate writeMask is to handle condition blocks that are self modifying
		for(int opIndex = beginop; opIndex < endop; ++opIndex)
		{
			Opcode & opcode = op(opIndex);
			std::cout << "op=" << opcode.opname();
			int argCount = opcode.nargs();
			
			const Symbol * symbolsReadByOp[argCount];
			int symbolsRead = 0;
			const Symbol * symbolsWrittenByOp[argCount];
			int symbolsWritten = 0;
			for(int argIndex = 0; argIndex < argCount; ++argIndex) {
				const Symbol * aSymbol = opargsym (opcode, argIndex);
				if (opcode.argwrite(argIndex)) {
					std::cout << " write to ";
					symbolsWrittenByOp[symbolsWritten++] = aSymbol;
				}
				if (opcode.argread(argIndex)) {
					symbolsReadByOp[symbolsRead++] = aSymbol;
					std::cout << " read from ";					
				}
				std::cout << " " << aSymbol->name();
	
				std::cout << " discovery " << aSymbol->name()  << std::endl;
				// Initially let all symbols be uniform 
				// so we get proper cascading of all dependencies
				// when we feed forward from varying shader globals, output parameters, and connected parameters
				constexpr bool isUniform = true;
				m_is_uniform_by_symbol[aSymbol] = isUniform;
			}
			std::cout << std::endl;

			for(int readIndex=0; readIndex < symbolsRead; ++readIndex) {
				const Symbol * symbolReadFrom = symbolsReadByOp[readIndex];
				for(int writeIndex=0; writeIndex < symbolsWritten; ++writeIndex) {
					const Symbol * symbolWrittenTo = symbolsWrittenByOp[writeIndex];
					// Skip self dependencies
					if (symbolWrittenTo != symbolReadFrom) {
						symbolFeedForwardMap.insert(std::make_pair(symbolReadFrom, symbolWrittenTo));
					}
				}		
				
				ensureWritesAtLowerDepthAreMasked(symbolReadFrom, blockDepth, maskId);
			}
			
			for(int writeIndex=0; writeIndex < symbolsWritten; ++writeIndex) {
				const Symbol * symbolWrittenTo = symbolsWrittenByOp[writeIndex];
				UsageInfo & info = usageInfoBySymbol[symbolWrittenTo];
				info.last_depth = writeBlockDepth;
				info.last_maskId = writeMaskId;
				info.potentially_unmasked_ops.push_back(std::make_pair(writeBlockDepth, opIndex));
			}
			
			// Add dependencies between symbols written to in this basic block
			// to the set of symbols the code blocks where dependent upon to be executed
			for(const Symbol *symbolCurrentBlockDependsOn : symbolsCurrentBlockDependsOn)
			{
				for(int writeIndex=0; writeIndex < symbolsWritten; ++writeIndex) {
					const Symbol * symbolWrittenTo = symbolsWrittenByOp[writeIndex];
					// Skip self dependencies
					if (symbolWrittenTo != symbolCurrentBlockDependsOn) {
						symbolFeedForwardMap.insert(std::make_pair(symbolCurrentBlockDependsOn, symbolWrittenTo));
					}
				}									
			}
			
			if (opcode.jump(0) >= 0)
			{
				// The operation with a jump depends on reading the follow symbols
				// track them for the following basic blocks as the writes
				// within those basic blocks will depend on the uniformity of 
				// the values read by this operation
				std::function<void()> pushSymbolsCurentBlockDependsOn;
				pushSymbolsCurentBlockDependsOn = [&]()->void {
					// Only coding for a single conditional variable
					ASSERT(symbolsRead == 1);			    		
					symbolsCurrentBlockDependsOn.push_back(symbolsReadByOp[0]);
				};
				
				std::function<void()> popSymbolsCurentBlockDependsOn;
				popSymbolsCurentBlockDependsOn = [&]()->void {			    			
					// Now that we have processed the dependent basic blocks
					// we continue processing instructions and those will no
					// longer be dependent on this operations read symbols

					// Only coding for a single conditional variable
					ASSERT(symbolsRead == 1);			    		
					ASSERT(symbolsCurrentBlockDependsOn.back() == symbolsReadByOp[0]);
					symbolsCurrentBlockDependsOn.pop_back();
				};
				
								
				// op must have jumps, therefore have nested code we need to process
				// We need to process these in the same order as the code generator
				// so our "block depth" lines up for symbol lookups
				if (opcode.opname() == op_if)
				{
					pushSymbolsCurentBlockDependsOn();
					// Then block
					std::cout << " THEN BLOCK BEGIN" << std::endl;
					int thenBlockDepth = blockDepth+1;
					int thenMaskId = nextMaskId++;
					discoverSymbolsBetween(opIndex+1, opcode.jump(0), thenBlockDepth, thenBlockDepth, thenMaskId, thenMaskId);
					std::cout << " THEN BLOCK END" << std::endl;
					// else block
					std::cout << " ELSE BLOCK BEGIN" << std::endl;
					int elseBlockDepth = blockDepth+1;
					int elseMaskId = nextMaskId++;
					discoverSymbolsBetween(opcode.jump(0), opcode.jump(1), elseBlockDepth, elseBlockDepth, elseMaskId, elseMaskId);
					std::cout << " ELSE BLOCK END" << std::endl;
					
					popSymbolsCurentBlockDependsOn();
					
				} else if ((opcode.opname() == op_for) || (opcode.opname() == op_while) || (opcode.opname() == op_dowhile))
				{
					// Init block
					// NOTE: init block doesn't depend on the for loops conditions and should be exempt
					std::cout << " FOR INIT BLOCK BEGIN" << std::endl;
					discoverSymbolsBetween(opIndex+1, opcode.jump(0), blockDepth, blockDepth, maskId, maskId);
					std::cout << " FOR INIT BLOCK END" << std::endl;

					int depthForBodyAndStep = blockDepth+1;
					int maskIdForBodyAndStep = nextMaskId++;
					pushSymbolsCurentBlockDependsOn();
					
					// Only coding for a single conditional variable
					ASSERT(symbolsRead == 1);
					loopControlFlowSymbolStack.push_back(symbolsReadByOp[0]);
					
					
					// Body block
					std::cout << " FOR BODY BLOCK BEGIN" << std::endl;
					discoverSymbolsBetween(opcode.jump(1), opcode.jump(2), depthForBodyAndStep, depthForBodyAndStep, maskIdForBodyAndStep, maskIdForBodyAndStep);
					std::cout << " FOR BODY BLOCK END" << std::endl;
										
					// Step block
					// Because the number of times the step block is executed depends on
					// when the loop condition block returns false, that means if 
					// the loop condition block is varying, then so would the condition block
					std::cout << " FOR STEP BLOCK BEGIN" << std::endl;
					discoverSymbolsBetween(opcode.jump(2), opcode.jump(3), depthForBodyAndStep, depthForBodyAndStep, maskIdForBodyAndStep, maskIdForBodyAndStep);
					std::cout << " FOR STEP BLOCK END" << std::endl;

					
				
					// Condition block
					// NOTE: Processing condition like it was a do/while
					// Although the first execution of the condition doesn't depend on the for loops conditions 
					// subsequent executions will depend on it on the previous loop's mask
					// We are processing the condition block out of order so that
					// any writes to any symbols it depends on can be marked first
					std::cout << " FOR COND BLOCK BEGIN" << std::endl;
					discoverSymbolsBetween(opcode.jump(0), opcode.jump(1), blockDepth, depthForBodyAndStep, maskId, maskIdForBodyAndStep);
					std::cout << " FOR COND BLOCK END" << std::endl;

					// Special case for symbols that are conditions
					// because we will be doing horizontal operations on these
					// to check if they are all 'false' to be able to stop
					// executing the loop, we need any writes to the
					// condition to be masked
					const Symbol * condition = opargsym (opcode, 0);
					ensureWritesAtLowerDepthAreMasked(condition, blockDepth, maskId);
					
					popSymbolsCurentBlockDependsOn();
					ASSERT(loopControlFlowSymbolStack.back() == symbolsReadByOp[0]);
					loopControlFlowSymbolStack.pop_back();

					
				} else if (opcode.opname() == op_functioncall)
				{
					// Function call itself operates on the same symbol dependencies
					// as the current block, there was no conditionals involved
					std::cout << " FUNCTION CALL BLOCK BEGIN" << std::endl;
					discoverSymbolsBetween(opIndex+1, opcode.jump(0), blockDepth, writeBlockDepth, maskId, writeMaskId);
					std::cout << " FUNCTION CALL BLOCK END" << std::endl;
					
				} else {
					ASSERT(0 && "Unhandled OSL instruction which contains jumps, note this uniform detection code needs to walk the code blocks identical to build_llvm_code");
				}

			}
			if (opcode.opname() == op_break)
			{
				// The break will need change the loop control flow which is dependent upon
				// a conditional.  By making a circular dependency between the break operation
				// and the conditionals value, any varying values in the conditional controlling 
				// the break should flow back to the loop control variable, which might need to
				// be varying so allow lanes to terminate the loop independently
				ASSERT(false == loopControlFlowSymbolStack.empty());
				const Symbol * loopCondition = loopControlFlowSymbolStack.back();
				
				// Now that last loop control condition should exist in our stack of symbols that
				// the current block with depends upon, we only need to add dependencies to the loop control
				// to conditionas inside the loop
				auto conditionIter = std::find(symbolsCurrentBlockDependsOn.begin(), symbolsCurrentBlockDependsOn.end(), loopCondition);
				ASSERT(conditionIter != symbolsCurrentBlockDependsOn.end());
				while(++conditionIter != symbolsCurrentBlockDependsOn.end()) {
					const Symbol * conditionBreakDependsOn =  *conditionIter;
					std::cout << ">>>Loop Conditional " << loopCondition->name().c_str() << " needs to depend on conditional " << conditionBreakDependsOn->name().c_str() << std::endl;
					symbolFeedForwardMap.insert(std::make_pair(conditionBreakDependsOn, loopCondition));
				}

				// Also update the usageInfo for the loop conditional to mark it as being written to
				// by the break operation (which it would be in varying scenario
				UsageInfo & info = usageInfoBySymbol[loopCondition];
				if (writeBlockDepth > info.last_depth)
				{
					info.last_depth = writeBlockDepth;
					info.last_maskId = writeMaskId;
				}
				info.potentially_unmasked_ops.push_back(std::make_pair(writeBlockDepth, opIndex));
			}
            if (opcode.opname() == op_getattribute)
            {
                for(int writeIndex=0; writeIndex < symbolsWritten; ++writeIndex) {
                    const Symbol * symbolWrittenTo = symbolsWrittenByOp[writeIndex];
                    symbolsWrittenToByGetAttribute.push_back(symbolWrittenTo);
                }
            }
			
			// If the op we coded jumps around, skip past its recursive block
			// executions.
			int next = opcode.farthest_jump ();
			if (next >= 0)
				opIndex = next-1;				
		}
	};
	
	int mainMask = nextMaskId++;

	// NOTE:  The order symbols are discovered should match the flow
	// of build_llvm_code calls coming from build_llvm_instance 
	// And build_llvm_code is called indirectly throught llvm_assign_initial_value.
	
	// TODO: not sure the main scope should be at a deepr scoope than the init operations 
	// for symbols.  I think they should be fine
	for (auto&& s : inst()->symbols()) {    	
		// Skip constants -- we always inline scalar constants, and for
		// array constants we will just use the pointers to the copy of
		// the constant that belongs to the instance.
		if (s.symtype() == SymTypeConst)
			continue;
		// Skip structure placeholders
		if (s.typespec().is_structure())
			continue;
		// Set initial value for constants, closures, and strings that are
		// not parameters.
		if (s.symtype() != SymTypeParam && s.symtype() != SymTypeOutputParam &&
			s.symtype() != SymTypeGlobal &&
			(s.is_constant() || s.typespec().is_closure_based() ||
			 s.typespec().is_string_based() || 
			 ((s.symtype() == SymTypeLocal || s.symtype() == SymTypeTemp)
			  && shadingsys().debug_uninit())))
		{
			if (s.has_init_ops() && s.valuesource() == Symbol::DefaultVal) {
				// Handle init ops.
				discoverSymbolsBetween(s.initbegin(), s.initend(), 0, 0, mainMask, mainMask);
			}
		}
	}
	
	// make a second pass for the parameters (which may make use of
	// locals and constants from the first pass)
	FOREACH_PARAM (Symbol &s, inst()) {
		// Skip structure placeholders
		if (s.typespec().is_structure())
			continue;
		// Skip if it's never read and isn't connected
		if (! s.everread() && ! s.connected_down() && ! s.connected()
			  && ! s.renderer_output())
			continue;
		// Skip if it's an interpolated (userdata) parameter and we're
		// initializing them lazily.
		if (s.symtype() == SymTypeParam
				&& ! s.lockgeom() && ! s.typespec().is_closure()
				&& ! s.connected() && ! s.connected_down()
				&& shadingsys().lazy_userdata())
			continue;
		// Set initial value for params (may contain init ops)
		if (s.has_init_ops() && s.valuesource() == Symbol::DefaultVal) {
			// Handle init ops.
			discoverSymbolsBetween(s.initbegin(), s.initend(), 0, 0, mainMask, mainMask);
		}
	}    	
	
	discoverSymbolsBetween(inst()->maincodebegin(), inst()->maincodeend(), 0, 0, mainMask, mainMask);
	
	// Now that all of the instructions have been discovered, we need to
	// make sure any writes to the output parameters that happened at 
	// lower depths are masked, as there may be no actual instruction
	// that reads the output variables at the outtermost scope
	// we will simulate that right here
	FOREACH_PARAM (Symbol &s, inst()) {
		// Skip structure placeholders
		if (s.typespec().is_structure())
			continue;
		// Skip if it's never read and isn't connected
		if (! s.everread() && ! s.connected_down() && ! s.connected()
			  && ! s.renderer_output())
			continue;
		if (s.symtype() == SymTypeOutputParam) {
			ensureWritesAtLowerDepthAreMasked(&s, 0, mainMask);
		}
	}    	
	
	std::cout << "About to build m_is_uniform_by_symbol" << std::endl;			
	
	std::function<void(const Symbol *)> recursivelyMarkNonUniform;
	recursivelyMarkNonUniform = [&](const Symbol* nonUniformSymbol)->void
	{
		bool previously_was_uniform = m_is_uniform_by_symbol[nonUniformSymbol];
		m_is_uniform_by_symbol[nonUniformSymbol] = false;
		if (previously_was_uniform) {
			auto range = symbolFeedForwardMap.equal_range(nonUniformSymbol);
			auto iter = range.first;
			for(;iter != range.second; ++iter) {
				const Symbol * symbolWrittenTo = iter->second;
				recursivelyMarkNonUniform(symbolWrittenTo);
			};
		}
	};
	
	auto endOfFeeds = symbolFeedForwardMap.end();	
	for(auto feedIter = symbolFeedForwardMap.begin();feedIter != endOfFeeds; )
	{
		const Symbol * symbolReadFrom = feedIter->first;
		//std::cout << " " << symbolReadFrom->name() << " feeds into " << symbolWrittenTo->name() << std::endl;
		
		bool is_uniform = true;			
		auto symType = symbolReadFrom->symtype();
		if (symType == SymTypeGlobal) {
			is_uniform = IsShaderGlobalUniformByName(symbolReadFrom->name());
		} else if (symType == SymTypeParam) {
				// TODO: perhaps the connected params do not necessarily
				// need to be varying 
				is_uniform = false;
		} 
		if (is_uniform == false) {
			// So we have a symbol that is not uniform, so it will be a wide type
			// Thus anyone who depends on it will need to be wide as well.
			recursivelyMarkNonUniform(symbolReadFrom);
		}
		
		// The multimap may have multiple entries with the same key
		// And we only need to iterate over unique keys,
		// so skip any consecutive duplicate keys
		do {
			++feedIter;
		} while (feedIter != endOfFeeds && symbolReadFrom == feedIter->first);
	}

	// Mark all output parameters as varying to catch
	// output parameters written to by uniform variables, 
	// as nothing would have made them varying, however as 
	// we write directly into wide data, we need to mark it
	// as varying so that the code generation will promote the uniform value
	// to varying before writing
	FOREACH_PARAM (Symbol &s, inst()) {    	
		if (s.symtype() == SymTypeOutputParam) {
			recursivelyMarkNonUniform(&s);
		}    			
	}


    //std::cout << "symbolsWrittenToByGetAttribute begin" << std::endl;
    for(const Symbol *s: symbolsWrittenToByGetAttribute) {
        //std::cout << s->name() << std::endl;
        recursivelyMarkNonUniform(s);
    }
    //std::cout << "symbolsWrittenToByGetAttribute end" << std::endl;

	std::cout << "Emit m_is_uniform_by_symbol" << std::endl;			
	
	for(auto rIter = m_is_uniform_by_symbol.begin(); rIter != m_is_uniform_by_symbol.end(); ++rIter) {
		const Symbol * rSym = rIter->first;
		bool is_uniform = rIter->second;
		std::cout << "--->" << rSym << " " << rSym->name() << " is " << (is_uniform ? "UNIFORM" : "VARYING") << std::endl;			
	}
	std::cout << std::flush;		
	std::cout << "done discoverVaryingAndMaskingOfLayer" << std::endl;
	
	
	std::cout << "Emit m_requires_masking_by_layer_and_op_index" << std::endl;			
	
	auto & requires_masking_by_op_index = m_requires_masking_by_layer_and_op_index[layer()];
	
	int opCount = requires_masking_by_op_index.size();
	for(int opIndex=0; opIndex < opCount; ++opIndex) {
		if (requires_masking_by_op_index[opIndex])
		{
			Opcode & opcode = op(opIndex);
			std::cout << "---> inst#" << opIndex << " op=" << opcode.opname() << " requires MASKING" << std::endl;
		}
	}
	std::cout << std::flush;		
	std::cout << "done m_requires_masking_by_layer_and_op_index" << std::endl;

}
	
bool 
BackendLLVMWide::isSymbolUniform(const Symbol& sym)
{
	ASSERT(false == m_is_uniform_by_symbol.empty());
	
	auto iter = m_is_uniform_by_symbol.find(&sym);
	if (iter == m_is_uniform_by_symbol.end()) 
	{	// TODO:  Any symbols not involved in operations would be uniform
		// unless they are an output, but I think not just an output of an invidual
		// shader, but the output of the entire network
		std::cout << " undiscovered " << sym.name() << " initial isUniform=";
		if (sym.symtype() == SymTypeOutputParam) {
                //&& ! sym.lockgeom() && ! sym.typespec().is_closure()
                //&& ! sym.connected() && ! sym.connected_down())
			std::cout << false << std::endl;
			return false;
		}
		std::cout << true << std::endl;		
		return true;
	}
	
	bool is_uniform = iter->second;
	return is_uniform;
}

bool 
BackendLLVMWide::requiresMasking(int opIndex)
{
	ASSERT(m_requires_masking_by_layer_and_op_index[layer()].empty() == false);
	ASSERT(m_requires_masking_by_layer_and_op_index[layer()].size() > opIndex);
	return m_requires_masking_by_layer_and_op_index[layer()][opIndex];
}

void 
BackendLLVMWide::push_varying_loop_condition(Symbol *condition)
{
	// Self documenting that nullptr is expected and 
	// indicates current loop scope is not varying
	DASSERT(condition == nullptr || condition != nullptr);
	m_generated_loops_condition_stack.push_back(condition);
}

Symbol * 
BackendLLVMWide::varying_condition_of_innermost_loop() const
{
	ASSERT(false == m_generated_loops_condition_stack.empty());
	return m_generated_loops_condition_stack.back();	
}

void 
BackendLLVMWide::pop_varying_loop_condition()
{
	ASSERT(false == m_generated_loops_condition_stack.empty());
	Symbol * varying_loop_condition = m_generated_loops_condition_stack.back();
	m_generated_loops_condition_stack.pop_back();
	if (nullptr != varying_loop_condition) {
		// However many break statements executed,
		// we are leaving the scope of the loop
		// so we can go ahead and clear them out
		ll.clear_mask_break();
	}
}


llvm::Value *
BackendLLVMWide::llvm_alloca (const TypeSpec &type, bool derivs, bool is_uniform, bool forceBool,
                          const std::string &name)
{
	std::cout << "llvm_alloca " << name ;
    TypeDesc t = llvm_typedesc (type);
    int n = derivs ? 3 : 1;
    std::cout << "n=" << n << " t.size()=" << t.size();
    m_llvm_local_mem += t.size() * n;
    if (is_uniform)
    {
    	std::cout << " as UNIFORM " << std::endl ;
    	if (forceBool) {    		
    		return ll.op_alloca (ll.type_bool(), n, name);
    	} else {
    		return ll.op_alloca (t, n, name);
    	}
    } else {
    	std::cout << " as VARYING " << std::endl ;
    	if (forceBool) {    		
    		return ll.op_alloca (ll.type_wide_bool(), n, name);
    	} else {
    		return ll.wide_op_alloca (t, n, name);
    	}
    }
}



llvm::Value *
BackendLLVMWide::getOrAllocateLLVMSymbol (const Symbol& sym, bool forceBool)
{
    DASSERT ((sym.symtype() == SymTypeLocal || sym.symtype() == SymTypeTemp ||
              sym.symtype() == SymTypeConst)
             && "getOrAllocateLLVMSymbol should only be for local, tmp, const");
    Symbol* dealiased = sym.dealias();
    std::string mangled_name = dealiased->mangled();
    AllocationMap::iterator map_iter = named_values().find(mangled_name);

    if (map_iter == named_values().end()) {
    	bool is_uniform = isSymbolUniform(sym);
    	
        llvm::Value* a = llvm_alloca (sym.typespec(), sym.has_derivs(), is_uniform, forceBool, mangled_name);
        named_values()[mangled_name] = a;
        return a;
    }
    return map_iter->second;
}



llvm::Value *
BackendLLVMWide::llvm_get_pointer (const Symbol& sym, int deriv,
                               llvm::Value *arrayindex)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Return NULL for request for pointer to derivs that don't exist
        return ll.ptr_cast (ll.void_ptr_null(),
        					ll.type_ptr (llvm_type(sym.typespec().elementtype())));
    }

    llvm::Value *result = NULL;
    if (sym.symtype() == SymTypeConst) {
        // For constants, start with *OUR* pointer to the constant values.
        result = ll.ptr_cast (ll.constant_ptr (sym.data()),
        						// Constants by definition should always be UNIFORM
        						ll.type_ptr (llvm_type(sym.typespec().elementtype())));

    } else {
        // Start with the initial pointer to the variable's memory location
        result = getLLVMSymbolBase (sym);
    	std::cerr << " llvm_get_pointer(" << sym.name() << ") result=";
    	ll.llvm_typeof(result)->dump();
    	std::cerr << std::endl;
        
    }
    if (!result)
        return NULL;  // Error

    // If it's an array or we're dealing with derivatives, step to the
    // right element.
    TypeDesc t = sym.typespec().simpletype();
    if (t.arraylen || has_derivs) {
    	std::cout << "llvm_get_pointer we're dealing with derivatives<-------" << std::endl;
    	std::cout << "arrayindex=" << arrayindex << " deriv=" << deriv << " t.arraylen="  << t.arraylen; 
    	std::cout << " isSymbolUniform="<< isSymbolUniform(sym) << std::endl;
    	
        int d = deriv * std::max(1,t.arraylen);
        if (arrayindex)
            arrayindex = ll.op_add (arrayindex, ll.constant(d));
        else
            arrayindex = ll.constant(d);
        result = ll.GEP (result, arrayindex);
    }

    return result;
}

llvm::Value *
BackendLLVMWide::llvm_load_value (const Symbol& sym, int deriv,
                                   llvm::Value *arrayindex, int component,
                                   TypeDesc cast, bool op_is_uniform)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Regardless of what object this is, if it doesn't have derivs but
        // we're asking for them, return 0.  Integers don't have derivs
        // so we don't need to worry about that case.
    	if (op_is_uniform) {
    		return ll.constant (0.0f);
    	} else {
    		return ll.wide_constant (0.0f);
    	}
    	
    }

    // arrayindex should be non-NULL if and only if sym is an array
    ASSERT (sym.typespec().is_array() == (arrayindex != NULL));

    if (sym.is_constant() && !sym.typespec().is_array() && !arrayindex) {
        // Shortcut for simple constants
        if (sym.typespec().is_float()) {
            if (cast == TypeDesc::TypeInt)
            	if (op_is_uniform) {
            		return ll.constant ((int)*(float *)sym.data());
            	} else {
                    return ll.wide_constant ((int)*(float *)sym.data());            		
            	}
            else
            	if (op_is_uniform) {
            		return ll.constant (*(float *)sym.data());
            	} else {
                    return ll.wide_constant (*(float *)sym.data());            		
            	}
        }
        if (sym.typespec().is_int()) {
            if (cast == TypeDesc::TypeFloat)
            	if (op_is_uniform) {
            		return ll.constant ((float)*(int *)sym.data());
            	} else {
                    return ll.wide_constant ((float)*(int *)sym.data());            		
            	}
            else
            {
            	if (op_is_uniform) {
            		return ll.constant (*(int *)sym.data());
            	} else {
                    return ll.wide_constant (*(int *)sym.data());            		
            	}
            }
        }
        if (sym.typespec().is_triple() || sym.typespec().is_matrix()) {
        	if (op_is_uniform) {
        		return ll.constant (((float *)sym.data())[component]);
        	} else {
                return ll.wide_constant (((float *)sym.data())[component]);        		
        	}
        }
        if (sym.typespec().is_string()) {
			// TODO:  NOT SURE WHAT TO DO WITH VARYING STRING
            //return ll.wide_constant (*(ustring *)sym.data());
        	ASSERT(op_is_uniform);
            return ll.constant (*(ustring *)sym.data());
        }
        ASSERT (0 && "unhandled constant type");
    }

    std::cout << "  llvm_load_value " << sym.typespec().string() << " cast " << cast << std::endl;
    return llvm_load_value (llvm_get_pointer (sym), sym.typespec(),
                            deriv, arrayindex, component, cast, op_is_uniform);
}



llvm::Value *
BackendLLVMWide::llvm_load_value (llvm::Value *ptr, const TypeSpec &type,
                                   int deriv, llvm::Value *arrayindex,
                                   int component, TypeDesc cast, bool op_is_uniform)
{
    if (!ptr)
        return NULL;  // Error

    // If it's an array or we're dealing with derivatives, step to the
    // right element.
    TypeDesc t = type.simpletype();
    if (t.arraylen || deriv) {
        int d = deriv * std::max(1,t.arraylen);
        if (arrayindex)
            arrayindex = ll.op_add (arrayindex, ll.constant(d));
        else
            arrayindex = ll.constant(d);
        ptr = ll.GEP (ptr, arrayindex);
    }

    // If it's multi-component (triple or matrix), step to the right field
    if (! type.is_closure_based() && t.aggregate > 1)
    {
    	std::cout << "step to the right field " << component << std::endl;
        ptr = ll.GEP (ptr, 0, component);
    }

    // Now grab the value
    llvm::Value *result = ll.op_load (ptr);

    if (type.is_closure_based())
        return result;

    // We may have bool masquarading as int's and need to promote them for
    // use in any int arithmetic
    if (type.is_int() &&
        (ll.llvm_typeof(result) == ll.type_wide_bool())) {
        if(cast == TypeDesc::TypeInt)
        {
            result = ll.op_bool_to_int(result);
        } else if (cast == TypeDesc::TypeFloat)
        {
            result = ll.op_bool_to_float(result);
        }
    }
    // Handle int<->float type casting
    if (type.is_floatbased() && cast == TypeDesc::TypeInt)
        result = ll.op_float_to_int (result);
    else if (type.is_int() && cast == TypeDesc::TypeFloat)
        result = ll.op_int_to_float (result);
	
	if (!op_is_uniform) { 
    	// TODO:  remove this assert once we have confirmed correct handling off all the
    	// different data types.  Using assert as a checklist to verify what we have 
    	// handled so far during development
    	ASSERT(cast == TypeDesc::UNKNOWN || cast == TypeDesc::TypeColor || cast == TypeDesc::TypeVector || cast == TypeDesc::TypePoint || cast == TypeDesc::TypeFloat || cast == TypeDesc::TypeInt);
    	
    	if (ll.llvm_typeof(result) ==  ll.type_float()) {
            result = ll.widen_value(result);    		    		
    	} else if (ll.llvm_typeof(result) ==  ll.type_triple()) {
            result = ll.widen_value(result);    		    		
    	} else if (ll.llvm_typeof(result) ==  ll.type_int()) {
            result = ll.widen_value(result);    		    		
    	} else {
        	ASSERT((ll.llvm_typeof(result) ==  ll.type_wide_float()) ||
        		   (ll.llvm_typeof(result) ==  ll.type_wide_int()) ||
        		   (ll.llvm_typeof(result) ==  ll.type_wide_triple()) ||
        		   (ll.llvm_typeof(result) ==  ll.type_wide_bool()));
    	}
    }

    return result;
}



llvm::Value *
BackendLLVMWide::llvm_load_constant_value (const Symbol& sym, 
                                       int arrayindex, int component,
                                       TypeDesc cast,
									   bool op_is_uniform)
{
    ASSERT (sym.is_constant() &&
            "Called llvm_load_constant_value for a non-constant symbol");

    // set array indexing to zero for non-arrays
    if (! sym.typespec().is_array())
        arrayindex = 0;
    ASSERT (arrayindex >= 0 &&
            "Called llvm_load_constant_value with negative array index");

    if (sym.typespec().is_float()) {
        const float *val = (const float *)sym.data();
        if (cast == TypeDesc::TypeInt)
        	if (op_is_uniform) {
        		return ll.constant ((int)val[arrayindex]);
        	} else {
        		return ll.wide_constant ((int)val[arrayindex]);        		
        	}
        else
        	if (op_is_uniform) {
        		return ll.constant (val[arrayindex]);
        	} else
        	{
                return ll.wide_constant (val[arrayindex]);        		
        	}
    }
    if (sym.typespec().is_int()) {
        const int *val = (const int *)sym.data();
        if (cast == TypeDesc::TypeFloat)
        	if (op_is_uniform) {
        		return ll.constant ((float)val[arrayindex]);
        	} else {
                return ll.wide_constant ((float)val[arrayindex]);        		
        	}
        else
        	if (op_is_uniform) {
        		return ll.constant (val[arrayindex]);
        	} else {
                return ll.wide_constant (val[arrayindex]);        		
        	}
    }
    if (sym.typespec().is_triple() || sym.typespec().is_matrix()) {
        const float *val = (const float *)sym.data();
        int ncomps = (int) sym.typespec().aggregate();
    	if (op_is_uniform) {
    		return ll.constant (val[ncomps*arrayindex + component]);
    	} else {
            return ll.wide_constant (val[ncomps*arrayindex + component]);
    	}
    }
    if (sym.typespec().is_string()) {
        const ustring *val = (const ustring *)sym.data();
    	if (op_is_uniform) {
    		return ll.constant (val[arrayindex]);
    	} else {
            return ll.wide_constant (val[arrayindex]);    		
    	}
    }

    ASSERT (0 && "unhandled constant type");
    return NULL;
}



llvm::Value *
BackendLLVMWide::llvm_load_component_value (const Symbol& sym, int deriv,
                                             llvm::Value *component)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Regardless of what object this is, if it doesn't have derivs but
        // we're asking for them, return 0.  Integers don't have derivs
        // so we don't need to worry about that case.
        ASSERT (sym.typespec().is_floatbased() && 
                "can't ask for derivs of an int");
		// TODO:  switching back to non-wide to figure out uniform vs. varying data
        //return ll.wide_constant (0.0f);
        return ll.constant (0.0f);
    }

    // Start with the initial pointer to the value's memory location
    llvm::Value* result = llvm_get_pointer (sym, deriv);
    if (!result)
        return NULL;  // Error

    TypeDesc t = sym.typespec().simpletype();
    ASSERT (t.aggregate != TypeDesc::SCALAR);
    // cast the Vec* to a float*
    result = ll.ptr_cast (result, ll.type_float_ptr());
    result = ll.GEP (result, component);  // get the component

    // Now grab the value
    return ll.op_load (result);
}



llvm::Value *
BackendLLVMWide::llvm_load_arg (const Symbol& sym, bool derivs, bool op_is_uniform)
{
    ASSERT (sym.typespec().is_floatbased());
    if (sym.typespec().is_int() ||
        (sym.typespec().is_float() && !derivs)) {
        // Scalar case

    	// If we are not uniform, then the argument should
    	// get passed as a pointer intstead of by value
    	// So let this case fall through
    	// NOTE:  Unclear of behavior if symbol is a constant
    	if (op_is_uniform) {
    		return llvm_load_value (sym, op_is_uniform);
    	} else if (sym.symtype() == SymTypeConst) {
    		// As the case to deliver a pointer to a symbol data
    		// doesn't provide an opportunity to promote a uniform constant
    		// to a wide value that the non-uniform function is expecting
    		// we will handle it here.
    		llvm::Value * wide_constant_value = llvm_load_constant_value (sym, 0, 0, TypeDesc::UNKNOWN, op_is_uniform);
    		
    		// Have to have a place on the stack for the pointer to the wide constant to point to
            const TypeSpec &t = sym.typespec();
            llvm::Value *tmpptr = llvm_alloca (t, true, op_is_uniform);
            
            // Store our wide pointer on the stack
            llvm_store_value (wide_constant_value, tmpptr, t, 0, NULL, 0);
    												
            // return pointer to our stacked wide constant
            return ll.void_ptr (tmpptr);    		
    	}
    }

    if (derivs && !sym.has_derivs()) {
        // Manufacture-derivs case
        const TypeSpec &t = sym.typespec();
    	
        // Copy the non-deriv values component by component
        llvm::Value *tmpptr = llvm_alloca (t, true, op_is_uniform);
        for (int c = 0;  c < t.aggregate();  ++c) {
            llvm::Value *v = llvm_load_value (sym, 0, c, TypeDesc::UNKNOWN, op_is_uniform);
            llvm_store_value (v, tmpptr, t, 0, NULL, c);
        }
        // Zero out the deriv values
        llvm::Value *zero;
        if (op_is_uniform)
            zero = ll.constant (0.0f);
        else
        	zero = ll.wide_constant (0.0f);
        for (int c = 0;  c < t.aggregate();  ++c)
            llvm_store_value (zero, tmpptr, t, 1, NULL, c);
        for (int c = 0;  c < t.aggregate();  ++c)
            llvm_store_value (zero, tmpptr, t, 2, NULL, c);
        return ll.void_ptr (tmpptr);
    }

    // Regular pointer case
    return llvm_void_ptr (sym);
}



bool
BackendLLVMWide::llvm_store_value (llvm::Value* new_val, const Symbol& sym,
                                    int deriv, llvm::Value* arrayindex,
                                    int component)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Attempt to store deriv in symbol that doesn't have it is just a nop
        return true;
    }

    return llvm_store_value (new_val, llvm_get_pointer (sym), sym.typespec(),
                             deriv, arrayindex, component);
}



bool
BackendLLVMWide::llvm_store_value (llvm::Value* new_val, llvm::Value* dst_ptr,
                                    const TypeSpec &type,
                                    int deriv, llvm::Value* arrayindex,
                                    int component)
{
    if (!dst_ptr)
        return false;  // Error

    // If it's an array or we're dealing with derivatives, step to the
    // right element.
    TypeDesc t = type.simpletype();
    if (t.arraylen || deriv) {
        int d = deriv * std::max(1,t.arraylen);
        if (arrayindex)
            arrayindex = ll.op_add (arrayindex, ll.constant(d));
        else
            arrayindex = ll.constant(d);
        dst_ptr = ll.GEP (dst_ptr, arrayindex);
    }

    // If it's multi-component (triple or matrix), step to the right field
    if (! type.is_closure_based() && t.aggregate > 1)
        dst_ptr = ll.GEP (dst_ptr, 0, component);

#if 1
    // TODO:  This check adds overhead, choose to remove (or not) later
    if(ll.type_ptr(ll.llvm_typeof(new_val)) != ll.llvm_typeof(dst_ptr))
    {
    	std::cerr << " new_val type=";
    	assert(0);
    	ll.llvm_typeof(new_val)->dump();
    	std::cerr << " dest_ptr type=";
    	ll.llvm_typeof(dst_ptr)->dump();
    	std::cerr << std::endl;
    }
    ASSERT(ll.type_ptr(ll.llvm_typeof(new_val)) == ll.llvm_typeof(dst_ptr));
#endif
    
    
    // Finally, store the value.
    ll.op_store (new_val, dst_ptr);
    return true;
}



bool
BackendLLVMWide::llvm_store_component_value (llvm::Value* new_val,
                                              const Symbol& sym, int deriv,
                                              llvm::Value* component)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Attempt to store deriv in symbol that doesn't have it is just a nop
        return true;
    }

    // Let llvm_get_pointer do most of the heavy lifting to get us a
    // pointer to where our data lives.
    llvm::Value *result = llvm_get_pointer (sym, deriv);
    if (!result)
        return false;  // Error

    TypeDesc t = sym.typespec().simpletype();
    ASSERT (t.aggregate != TypeDesc::SCALAR);
    // cast the Vec* to a float*
    result = ll.ptr_cast (result, ll.type_float_ptr());
    result = ll.GEP (result, component);  // get the component

    // Finally, store the value.
    ll.op_store (new_val, result);
    return true;
}



llvm::Value *
BackendLLVMWide::groupdata_field_ref (int fieldnum)
{
    return ll.GEP (groupdata_ptr(), 0, fieldnum);
}


llvm::Value *
BackendLLVMWide::groupdata_field_ptr (int fieldnum, TypeDesc type, bool is_uniform)
{
    llvm::Value *result = ll.void_ptr (groupdata_field_ref (fieldnum));
    if (type != TypeDesc::UNKNOWN) {
		if (is_uniform) {
			result = ll.ptr_to_cast (result, llvm_type(type));
		} else {
			result = ll.ptr_to_cast (result, llvm_wide_type(type));
		}
    }
    return result;
}


llvm::Value *
BackendLLVMWide::layer_run_ref (int layer)
{
    int fieldnum = 0; // field 0 is the layer_run array
    llvm::Value *layer_run = groupdata_field_ref (fieldnum);
    return ll.GEP (layer_run, 0, layer);
}



llvm::Value *
BackendLLVMWide::userdata_initialized_ref (int userdata_index)
{
    int fieldnum = 1; // field 1 is the userdata_initialized array
    llvm::Value *userdata_initiazlied = groupdata_field_ref (fieldnum);
    return ll.GEP (userdata_initiazlied, 0, userdata_index);
}


llvm::Value *
BackendLLVMWide::llvm_call_function (const char *name, 
                                      const Symbol **symargs, int nargs,
                                      bool deriv_ptrs,
                                      bool function_is_uniform,
                                      bool functionIsLlvmInlined,
                                      bool ptrToReturnStructIs1stArg)
{
    std::vector<llvm::Value *> valargs;
    valargs.resize ((size_t)nargs);
    for (int i = 0;  i < nargs;  ++i) {
        const Symbol &s = *(symargs[i]);
        const TypeSpec &t = s.typespec();

        if (t.is_closure())
            valargs[i] = llvm_load_value (s);
        else if (t.simpletype().aggregate > 1 ||
                (deriv_ptrs && s.has_derivs()) ||
                (!function_is_uniform && !functionIsLlvmInlined)
                 ) 
        {
        	// Need to pass a pointer to the function
        	if (function_is_uniform || (s.symtype() != SymTypeConst)) {
                valargs[i] = llvm_void_ptr (s);
        	} else {
            	std::cout << "....widening constant value " << s.name().c_str() << std::endl;

        		DASSERT(s.symtype() == SymTypeConst);
        		DASSERT(function_is_uniform);
        		// As the case to deliver a pointer to a symbol data
        		// doesn't provide an opportunity to promote a uniform constant
        		// to a wide value that the non-uniform function is expecting
        		// we will handle it here.
        		llvm::Value * wide_constant_value = llvm_load_constant_value (s, 0, 0, TypeDesc::UNKNOWN, function_is_uniform);
        		
        		// Have to have a place on the stack for the pointer to the wide constant to point to
                llvm::Value *tmpptr = llvm_alloca (t, true, function_is_uniform);
                
                // Store our wide pointer on the stack
                llvm_store_value (wide_constant_value, tmpptr, t, 0, NULL, 0);
        												
                // return pointer to our stacked wide constant
                valargs[i] =  ll.void_ptr (tmpptr);    		
        	}
        	
        	
        	std::cout << "....pushing " << s.name().c_str() << " as void_ptr"  << std::endl;
        }
        else
        {
        	std::cout << "....pushing " << s.name().c_str() << " as value" << std::endl;
            valargs[i] = llvm_load_value (s, /*deriv*/ 0, /*component*/ 0, TypeDesc::UNKNOWN, function_is_uniform);
        }
    }
    std::cout << "call_function " << name << std::endl;
    llvm::Value * func_call = ll.call_function (name, (valargs.size())? &valargs[0]: NULL,
                             (int)valargs.size());
    if (ptrToReturnStructIs1stArg)
    	ll.mark_structure_return_value(func_call);
    return func_call;
}



llvm::Value *
BackendLLVMWide::llvm_call_function (const char *name, const Symbol &A,
                                 bool deriv_ptrs)
{
    const Symbol *args[1];
    args[0] = &A;
    return llvm_call_function (name, args, 1, deriv_ptrs);
}



llvm::Value *
BackendLLVMWide::llvm_call_function (const char *name, const Symbol &A,
                                 const Symbol &B, bool deriv_ptrs)
{
    const Symbol *args[2];
    args[0] = &A;
    args[1] = &B;
    return llvm_call_function (name, args, 2, deriv_ptrs);
}



llvm::Value *
BackendLLVMWide::llvm_call_function (const char *name, const Symbol &A,
                                 const Symbol &B, const Symbol &C,
                                 bool deriv_ptrs)
{
    const Symbol *args[3];
    args[0] = &A;
    args[1] = &B;
    args[2] = &C;
    return llvm_call_function (name, args, 3, deriv_ptrs);
}



llvm::Value *
BackendLLVMWide::llvm_test_nonzero (Symbol &val, bool test_derivs)
{
    const TypeSpec &ts (val.typespec());
    ASSERT (! ts.is_array() && ! ts.is_closure() && ! ts.is_string());
    TypeDesc t = ts.simpletype();

    // Handle int case -- guaranteed no derivs, no multi-component
    if (t == TypeDesc::TypeInt) {

    	
    	// Because we allow temporaries and local results of comparison operations
    	// to use the native bool type of i1, we will need to build an matching constant 0
    	// for comparisons.  We can just interrogate the underlying llvm symbol to see if 
    	// it is a bool
    	llvm::Value * llvmValue = llvm_get_pointer (val);
    	//std::cout << "llvmValue type=" << ll.llvm_typenameof(llvmValue) << std::endl;
    	
    	if(ll.llvm_typeof(llvmValue) == ll.type_ptr(ll.type_bool())) {
    		return ll.op_ne (llvm_load_value(val), ll.constant_bool(0));
    	} else {
    		return ll.op_ne (llvm_load_value(val), ll.constant(0));
    	}
    	
    }

    // float-based
    int ncomps = t.aggregate;
    int nderivs = (test_derivs && val.has_derivs()) ? 3 : 1;
    llvm::Value *isnonzero = NULL;
    for (int d = 0;  d < nderivs;  ++d) {
        for (int c = 0;  c < ncomps;  ++c) {
            llvm::Value *v = llvm_load_value (val, d, c);
            llvm::Value *nz = ll.op_ne (v, ll.constant(0.0f), true);
            if (isnonzero)  // multi-component/deriv: OR with running result
                isnonzero = ll.op_or (nz, isnonzero);
            else
                isnonzero = nz;
        }
    }
    return isnonzero;
}



bool
BackendLLVMWide::llvm_assign_impl (Symbol &Result, Symbol &Src,
                                    int arrayindex)
{
    ASSERT (! Result.typespec().is_structure());
    ASSERT (! Src.typespec().is_structure());

    const TypeSpec &result_t (Result.typespec());
    const TypeSpec &src_t (Src.typespec());

    llvm::Value *arrind = arrayindex >= 0 ? ll.constant (arrayindex) : NULL;

    if (Result.typespec().is_closure() || Src.typespec().is_closure()) {
        if (Src.typespec().is_closure()) {
            llvm::Value *srcval = llvm_load_value (Src, 0, arrind, 0);
            llvm_store_value (srcval, Result, 0, arrind, 0);
        } else {
            llvm::Value *null = ll.constant_ptr(NULL, ll.type_void_ptr());
            llvm_store_value (null, Result, 0, arrind, 0);
        }
        return true;
    }

    if (Result.typespec().is_matrix() && Src.typespec().is_int_or_float()) {
        // Handle m=f, m=i separately
        llvm::Value *src = llvm_load_value (Src, 0, arrind, 0, TypeDesc::FLOAT /*cast*/);
        // m=f sets the diagonal components to f, the others to zero
        llvm::Value *zero = ll.constant (0.0f);
        for (int i = 0;  i < 4;  ++i)
            for (int j = 0;  j < 4;  ++j)
                llvm_store_value (i==j ? src : zero, Result, 0, arrind, i*4+j);
        llvm_zero_derivs (Result);  // matrices don't have derivs currently
        return true;
    }

    // Copying of entire arrays.  It's ok if the array lengths don't match,
    // it will only copy up to the length of the smaller one.  The compiler
    // will ensure they are the same size, except for certain cases where
    // the size difference is intended (by the optimizer).
    if (result_t.is_array() && src_t.is_array() && arrayindex == -1) {
        ASSERT (assignable(result_t.elementtype(), src_t.elementtype()));
        llvm::Value *resultptr = llvm_void_ptr (Result);
        llvm::Value *srcptr = llvm_void_ptr (Src);
        int len = std::min (Result.size(), Src.size());
        int align = result_t.is_closure_based() ? (int)sizeof(void*) :
                                       (int)result_t.simpletype().basesize();
        if (Result.has_derivs() && Src.has_derivs()) {
            ll.op_memcpy (resultptr, srcptr, 3*len, align);
        } else {
            ll.op_memcpy (resultptr, srcptr, len, align);
            if (Result.has_derivs())
                llvm_zero_derivs (Result);
        }
        return true;
    }

	bool is_uniform = isSymbolUniform(Result);
    // The following code handles f=f, f=i, v=v, v=f, v=i, m=m, s=s.
    // Remember that llvm_load_value will automatically convert scalar->triple.
    TypeDesc rt = Result.typespec().simpletype();
    TypeDesc basetype = TypeDesc::BASETYPE(rt.basetype);
    int num_components = rt.aggregate;
    for (int i = 0; i < num_components; ++i) {
    	llvm::Value* src_val ;
    	// Automatically handle widening the source value to match the destination's
		src_val = Src.is_constant()
			? llvm_load_constant_value (Src, arrayindex, i, basetype, is_uniform)
			: llvm_load_value (Src, 0, arrind, i, basetype, is_uniform);
        if (!src_val)
            return false;
        
        llvm_store_value (src_val, Result, 0, arrind, i);
    }

    // Handle derivatives
    if (Result.has_derivs()) {
        if (Src.has_derivs()) {
            // src and result both have derivs -- copy them
            for (int d = 1;  d <= 2;  ++d) {
                for (int i = 0; i < num_components; ++i) {
                    llvm::Value* val = llvm_load_value (Src, d, arrind, i);
                    llvm_store_value (val, Result, d, arrind, i);
                }
            }
        } else {
            // Result wants derivs but src didn't have them -- zero them
            llvm_zero_derivs (Result);
        }
    }
    return true;
}




}; // namespace pvt
OSL_NAMESPACE_EXIT