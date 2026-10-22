// Applies the fact files written by tools/*.py (build/facts/*.json) to the program.
// Re-runnable: names are applied as IMPORTED and never replace a USER_DEFINED name,
// tags and comments are idempotent.
//
// Usage (headless): -postScript ApplyFacts.java <facts dir>
//@category FrostLite
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.framework.Application;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.FileDataTypeManager;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.data.FunctionDefinitionDataType;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.Undefined;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Variable;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import java.io.File;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class ApplyFacts extends GhidraScript {
    private SymbolTable symbols;
    private int named, kept, created, tagged, labelled;
    // the variadic functions' fixed parameter counts (varargs.json), for applyProtos
    private final Map<Address, Integer> vaFixed = new HashMap<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args.length > 0 ? args[0] : "build/facts");
        symbols = currentProgram.getSymbolTable();

        // first, so no other fact lands on a function that is part of another
        File pc = new File(dir, "pieces.json");
        if (pc.exists()) applyPieces(read(pc));
        File lua = new File(dir, "luabind.json");
        if (lua.exists()) applyLua(read(lua));
        // C_ namespaces registered in code (same "functions" map: "C_X.Name" -> handler)
        File creg = new File(dir, "cregister.json");
        if (creg.exists()) applyLua(read(creg));
        File vt = new File(dir, "vtables.json");
        if (vt.exists()) applyVtables(read(vt));
        File src = new File(dir, "srcrefs.json");
        if (src.exists()) applySrcrefs(read(src));
        File imp = new File(dir, "imports.json");
        if (imp.exists()) applyImports(read(imp));
        File tags = new File(dir, "typetags.json");
        if (tags.exists()) applyTypeTags(read(tags));
        File rc = new File(dir, "retcheck.json");
        if (rc.exists()) applyRetcheck(read(rc));
        File nr = new File(dir, "noreturn.json");
        if (nr.exists()) applyNoReturn(read(nr));
        File va = new File(dir, "varargs.json");
        if (va.exists()) applyVarargs(read(va));
        // after the varargs (which keep their own fixed count)
        File sw = new File(dir, "switches.json");
        if (sw.exists()) applySwitches(read(sw));
        // (read before the prototypes, which give each function its keep convention)
        File pv = new File(dir, "preserved.json");
        if (pv.exists()) {
            JsonObject pvRoot = read(pv);
            // (with 'needed' in the file, its 'functions' are the chosen conventions, xmm0 among
            // them where a caller relies on it: protos.py's keep_xmm0 is folded in already)
            preservedChosen = pvRoot.has("needed");
            for (Map.Entry<String, JsonElement> e : pvRoot.getAsJsonObject("functions").entrySet()) {
                java.util.List<String> regs = new ArrayList<>();
                for (JsonElement r : e.getValue().getAsJsonArray()) regs.add(r.getAsString());
                preserved.put(addr(e.getKey()), regs);
            }
        }
        // (before the prototypes, which pin these positions to integer types)
        File ip = new File(dir, "intparams.json");
        if (ip.exists()) {
            for (Map.Entry<String, JsonElement> e : read(ip).getAsJsonObject("functions").entrySet()) {
                Set<Integer> at = new java.util.HashSet<>();
                for (JsonElement k : e.getValue().getAsJsonArray()) at.add(k.getAsInt());
                intParams.put(addr(e.getKey()), at);
            }
        }
        File pr = new File(dir, "protos.json");
        if (pr.exists()) {
            JsonObject protos = read(pr);
            applyProtos(protos);
            applyIntParamHomes(protos.getAsJsonObject("functions"));
            if (protos.has("stack_blocks")) applyStackBlocks(protos.getAsJsonObject("stack_blocks"));
        }
        File cs = new File(dir, "callsites.json");
        if (cs.exists()) applyCallsites(read(cs));
        // hand-identified names last, so they win over anything extracted
        File names = new File(dir, "names.json");
        if (names.exists()) applyNames(read(names));
        // after the names and varargs, so each slot takes its target's final signature
        File fp = new File(dir, "funcptrs.json");
        if (fp.exists()) applyFuncptrs(read(fp));
        // after the function-pointer slots, which keep their signature types
        File gp = new File(dir, "globalptrs.json");
        if (gp.exists()) applyGlobalPtrs(read(gp));
        uniqueNames();

        println(String.format("ApplyFacts: named %d (kept %d user names), created %d functions, " +
                "tagged %d, labelled %d vtables", named, kept, created, tagged, labelled));
    }

    private JsonObject read(File f) throws Exception {
        return JsonParser.parseString(Files.readString(f.toPath())).getAsJsonObject();
    }

    private Address addr(String hex) {
        return toAddr(Long.parseUnsignedLong(hex.startsWith("0x") ? hex.substring(2) : hex, 16));
    }

    // the piece and interior addresses (applyPieces): never a function of their own
    private final java.util.Set<Address> partOfAnother = new java.util.HashSet<>();

    private Function functionAt(Address a) {
        Function f = getFunctionAt(a);
        if (f == null && !partOfAnother.contains(a)) {
            CreateFunctionCmd cmd = new CreateFunctionCmd(a);
            if (cmd.applyTo(currentProgram, monitor)) {
                created++;
                f = getFunctionAt(a);
            }
        }
        return f;
    }

    private Namespace namespace(String path) throws Exception {
        Namespace ns = currentProgram.getGlobalNamespace();
        for (String part : path.split("::")) {
            Namespace next = symbols.getNamespace(part, ns);
            ns = next != null ? next : symbols.createNameSpace(ns, part, SourceType.IMPORTED);
        }
        return ns;
    }

    private void name(Function f, String ns, String name) throws Exception {
        if (f.getSymbol().getSource() == SourceType.USER_DEFINED) {
            kept++;
            return;
        }
        Namespace space = namespace(ns);
        // a name another function already has in the namespace gets this one's address: five
        // script object types each register their own CreateAnimationGroup, and with one name
        // between them a call in the decompiled text no longer says which function it calls (the
        // translator left 125 such Lua:: calls unresolved, C2065 'Lua', 2026-10-16)
        for (Symbol s : symbols.getSymbols(name, space)) {
            if (s.getObject() instanceof Function && !s.getAddress().equals(f.getEntryPoint())) {
                name = name + "_" + f.getEntryPoint().toString();
                break;
            }
        }
        f.setParentNamespace(space);
        f.setName(name, SourceType.IMPORTED);
        named++;
    }

    // protos.json "stack_blocks" (tools/protos.py): the stack blocks a function moves 16 bytes at a
    // time. Where the decompiler split one into two or more narrower locals, they become one byte
    // array of the block's size: split, a 32-byte copy out of a 4-byte `local_4e8` was dropped as
    // dead, and Ed25519's verify (FUN_1420bf4f0) compared R with a buffer never written (2026-10-20).
    private void applyStackBlocks(JsonObject blocks) throws Exception {
        int made = 0, functions = 0;
        for (Map.Entry<String, JsonElement> e : blocks.entrySet()) {
            Function f = getFunctionAt(addr(e.getKey()));
            if (f == null) continue;
            ghidra.program.model.listing.StackFrame sf = f.getStackFrame();
            boolean touched = false;
            for (JsonElement b : e.getValue().getAsJsonArray()) {
                int off = b.getAsJsonArray().get(0).getAsInt();
                int size = b.getAsJsonArray().get(1).getAsInt();
                List<Variable> inside = new ArrayList<>();
                boolean straddles = false;
                for (Variable v : sf.getStackVariables()) {
                    int vo = v.getStackOffset(), ve = vo + Math.max(1, v.getLength());
                    if (ve <= off || vo >= off + size) continue;
                    if (vo < off || ve > off + size) straddles = true;
                    inside.add(v);
                }
                // (none that a wider variable straddles, nor one a single variable already covers
                // whole; the decompiler's own split locals are not in the listing, so an empty
                // slot is the usual case)
                if (straddles || (inside.size() == 1 && inside.get(0).getStackOffset() == off
                        && inside.get(0).getLength() >= size)) continue;
                for (Variable v : inside) sf.clearVariable(v.getStackOffset());
                try {
                    sf.createVariable(String.format("blk_%x", -off), off,
                            new ArrayDataType(Undefined.getUndefinedDataType(1), size, 1), SourceType.USER_DEFINED);
                    made++;
                    touched = true;
                } catch (Exception ex) {
                    // (a parameter's slot, or another conflict: leave it)
                }
            }
            if (touched) functions++;
        }
        println("ApplyFacts: split stack blocks made arrays " + made + " in " + functions + " functions");
    }

    private void applyLua(JsonObject root) throws Exception {
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("functions").entrySet()) {
            Function f = functionAt(addr(e.getKey()));
            if (f == null) continue;
            String full = e.getValue().getAsString();   // "C_Container.GetBagName" or "UnitExists"
            int dot = full.lastIndexOf('.');
            String ns = dot < 0 ? "Lua" : "Lua::" + full.substring(0, dot);
            name(f, ns, dot < 0 ? full : full.substring(dot + 1));
        }
    }

    private void applyVtables(JsonObject root) throws Exception {
        for (JsonElement el : root.getAsJsonArray("vtables")) {
            JsonObject v = el.getAsJsonObject();
            Address a = addr(v.get("vtable").getAsString());
            int slots = v.getAsJsonArray("slots").size();
            String label = "vtable_" + a.toString();
            Symbol existing = getSymbolAt(a);
            if (existing == null || existing.getSource() == SourceType.DEFAULT
                    || existing.getSource() == SourceType.ANALYSIS) {
                createLabel(a, label, true, SourceType.IMPORTED);
                labelled++;
            }
            if (slots > 0) {
                try {
                    clearListing(a, a.add(8L * slots - 1));
                    createData(a, new ArrayDataType(PointerDataType.dataType, slots, 8));
                } catch (Exception ex) {
                    // leave whatever analysis put there
                }
                for (JsonElement s : v.getAsJsonArray("slots")) functionAt(addr(s.getAsString()));
            }
            StringBuilder sb = new StringBuilder("vtable, " + slots + " slots; loaded by");
            for (JsonElement r : v.getAsJsonArray("referenced_by")) sb.append(' ').append(r.getAsString());
            setPlateComment(a, sb.toString());
        }
    }

    // The Windows API prototypes Ghidra ships (Base/data/typeinfo/win32), by function name. The
    // definitions read through the archive, so it stays open until they are resolved.
    private Map<String, FunctionDefinition> windowsPrototypes(FileDataTypeManager archive) {
        Map<String, FunctionDefinition> out = new HashMap<>();
        Iterator<DataType> it = archive.getAllDataTypes();
        while (it.hasNext()) {
            DataType t = it.next();
            if (t instanceof FunctionDefinition) {
                out.putIfAbsent(t.getName(), (FunctionDefinition) t);
            }
        }
        return out;
    }

    // Import slots recovered from a live process by tools/iat_dump.py: label each slot
    // with its API name and type it as a pointer to the API's prototype, so `call [slot]`
    // reads as the API with its arguments. A bare pointer let the decompiler call
    // EnterCriticalSection with none (FUN_1415ae990: the bgs mutex lock, 2026-10-15).
    private void applyImports(JsonObject root) throws Exception {
        File gdt = Application.getModuleDataFile("Base", "typeinfo/win32/windows_vs12_64.gdt").getFile(false);
        FileDataTypeManager archive = FileDataTypeManager.openFileArchive(gdt, false);
        try {
            applyImports(root, windowsPrototypes(archive));
        } finally {
            archive.close();
        }
    }

    private void applyImports(JsonObject root, Map<String, FunctionDefinition> prototypes) throws Exception {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        int typed = 0, untyped = 0;
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("imports").entrySet()) {
            JsonObject i = e.getValue().getAsJsonObject();
            JsonElement n = i.get("imported_name");
            if (n == null || n.isJsonNull()) continue;
            // a list means folded identical code (htons/ntohs): keep every candidate
            String name;
            if (n.isJsonArray()) {
                StringBuilder sb = new StringBuilder();
                for (JsonElement x : n.getAsJsonArray()) {
                    if (sb.length() > 0) sb.append("_or_");
                    sb.append(x.getAsString());
                }
                name = sb.toString();
            } else {
                name = n.getAsString();
            }
            Address a = addr(e.getKey());
            String dll = i.get("dll").getAsString().toUpperCase();
            // folded identical code (htons_or_ntohs): the candidates share a prototype
            FunctionDefinition proto = prototypes.get(name.split("_or_")[0]);
            DataType slot = proto == null ? PointerDataType.dataType
                    : new PointerDataType(dtm.resolve(proto, DataTypeConflictHandler.DEFAULT_HANDLER), 8);
            try {
                clearListing(a, a.add(7));
                createData(a, slot);
                if (proto != null) typed++; else untyped++;
            } catch (Exception ex) {
                // keep analysis' typing
            }
            Symbol existing = getSymbolAt(a);
            if (existing != null && existing.getSource() == SourceType.USER_DEFINED) {
                kept++;
                continue;
            }
            createLabel(a, name, namespace("Import::" + dll.replace('.', '_')),
                    true, SourceType.IMPORTED);
            named++;
        }
        println("ApplyFacts: import slots typed with a Windows prototype " + typed + ", bare pointers " + untyped);
    }

    // funcptrs.json (tools/funcptrs.py): data slots holding a function's address that code calls
    // through. Typed as a pointer to the target's own signature, as the import slots are, so the
    // decompiler stops guessing each call's arguments from the registers it sees set: as a bare
    // pointer, curl's Curl_ccalloc slot (0x142d3e8b8 -> _calloc_base) came out as `(1)` with the
    // size dropped at about half of its calls (Curl_multi_handle, 2026-10-15).
    private void applyFuncptrs(JsonObject root) throws Exception {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        int typed = 0, unsigned = 0, occupied = 0;
        for (Map.Entry<String, JsonElement> e : root.entrySet()) {
            Address a = addr(e.getKey());
            Function f = getFunctionAt(addr(e.getValue().getAsJsonObject().get("target").getAsString()));
            if (f == null || f.getSignatureSource() == SourceType.DEFAULT) {
                // no signature to give it: an empty one would drop every argument
                unsigned++;
                continue;
            }
            Data d = getDataAt(a);
            if (d != null && d.isDefined() && !(d.getDataType() instanceof Pointer)
                    && !(d.getDataType() instanceof Undefined)) {
                // a struct or array someone typed: leave it
                occupied++;
                continue;
            }
            FunctionDefinitionDataType sig = new FunctionDefinitionDataType(f, true);
            sig.setName("fp_" + f.getName());
            DataType slot = new PointerDataType(dtm.resolve(sig, DataTypeConflictHandler.REPLACE_HANDLER), 8);
            try {
                clearListing(a, a.add(7));
                createData(a, slot);
                typed++;
            } catch (Exception ex) {
                occupied++;
            }
        }
        println(String.format("ApplyFacts: function-pointer slots typed %d, target without a signature %d, " +
                "left as typed %d", typed, unsigned, occupied));
    }

    // data/names.json: functions identified by hand, with the evidence as a comment.
    private void applyNames(JsonObject root) throws Exception {
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("functions").entrySet()) {
            Function f = functionAt(addr(e.getKey()));
            if (f == null) continue;
            JsonObject n = e.getValue().getAsJsonObject();
            name(f, n.has("ns") ? n.get("ns").getAsString() : "Global", n.get("name").getAsString());
            if (n.has("evidence")) setPlateComment(f.getEntryPoint(), "identified: " + n.get("evidence").getAsString());
        }
    }

    // typetags.json: the classes a function allocates or frees, as tags and a comment.
    private void applyTypeTags(JsonObject root) throws Exception {
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("functions").entrySet()) {
            Function f = getFunctionAt(addr(e.getKey()));
            if (f == null) continue;
            StringBuilder sb = new StringBuilder();
            for (JsonElement c : e.getValue().getAsJsonArray()) {
                if (sb.length() > 0) sb.append(", ");
                sb.append(c.getAsString());
            }
            String line = "types: " + sb;
            String old = f.getComment();
            if (old == null || !old.contains("types: ")) {
                f.setComment(old == null ? line : old + "\n" + line);
            }
            tagged++;
        }
    }

    // Tags are derived facts: when an extractor stops claiming a function (a fixed bug,
    // a verified reference), its old tag must go too. Removes every tag with this prefix
    // from functions outside `keep`.
    private int dropStaleTags(String prefix, java.util.Set<String> keep) {
        int dropped = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            String key = "0x" + Long.toHexString(f.getEntryPoint().getOffset());
            for (var tag : new java.util.ArrayList<>(f.getTags())) {
                if (tag.getName().startsWith(prefix) && !keep.contains(key)) {
                    f.removeTag(tag.getName());
                    dropped++;
                }
            }
        }
        return dropped;
    }

    // noreturn.json: functions that never return (tools/noreturn.py). The decompiler reads
    // the flag at decompile time, so callers stop showing code after these calls.
    private int noreturnSet;

    private void applyNoReturn(JsonObject root) throws Exception {
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("functions").entrySet()) {
            Function f = getFunctionAt(addr(e.getKey()));
            if (f == null || f.hasNoReturn()) continue;
            f.setNoReturn(true);
            noreturnSet++;
        }
        println("ApplyFacts: marked " + noreturnSet + " functions noreturn");
    }

    // varargs.json: variadic functions and their fixed parameter count (tools/varargs.py).
    // Marked varargs with only the fixed parameters, and the signature locked as IMPORTED so
    // the commit pass (CommitPrototypes.java, which re-derives only ANALYSIS and below) keeps
    // it: committed with all four register parameters, every caller passed its own r8/r9.
    private void applyVarargs(JsonObject root) throws Exception {
        int set = 0, user = 0, unset = 0;
        // a function an earlier session marked variadic that the facts no longer call one (the
        // text-based tools/varargs.py took a parameter passed on by reference for a va_start,
        // FUN_1417eec30's fourth, and its body decompiled `in_R9`): back to a fixed signature,
        // which applyProtos then gives the parameters the instructions read. Only what this script
        // marked (IMPORTED): FID's library signatures are ANALYSIS, user ones USER_DEFINED
        JsonObject want = root.getAsJsonObject("functions");
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.hasVarArgs() || f.getSignatureSource() != SourceType.IMPORTED) continue;
            if (want.has("0x" + f.getEntryPoint().toString())) continue;
            f.setVarArgs(false);
            f.setSignatureSource(SourceType.ANALYSIS);
            unset++;
        }
        println("ApplyFacts: no longer variadic " + unset);
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("functions").entrySet()) {
            Function f = getFunctionAt(addr(e.getKey()));
            if (f == null) continue;
            if (f.getSignatureSource() == SourceType.USER_DEFINED) {
                user++;
                continue;
            }
            int fixed = e.getValue().getAsInt();
            vaFixed.put(f.getEntryPoint(), fixed);
            Parameter[] cur = f.getParameters();
            if (f.hasVarArgs() && cur.length == fixed && f.getSignatureSource() == SourceType.IMPORTED) continue;
            // (fewer than the fixed count so far: the rest as plain registers, which applyProtos
            // then types; FUN_14253a9a0 takes six and starts its va_list past the sixth)
            List<Variable> params = new ArrayList<>();
            for (int i = 0; i < fixed; i++) {
                params.add(i < cur.length ? new ParameterImpl(cur[i].getName(), cur[i].getDataType(), currentProgram)
                        : new ParameterImpl("param_" + (i + 1), Undefined.getUndefinedDataType(8), currentProgram));
            }
            f.updateFunction(null, null, params, FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true,
                    SourceType.IMPORTED);
            f.setVarArgs(true);
            f.setSignatureSource(SourceType.IMPORTED);
            set++;
        }
        println("ApplyFacts: marked " + set + " functions variadic (kept " + user + " user signatures)");
    }

    // protos.json (tools/protos.py): each function's parameters as its instructions read them,
    // in place of the Decompiler Parameter ID guesses CommitPrototypes.java used to commit
    // (FUN_1414fc8b0 reads dx and r8w and was committed as `(uint)`: its body then read registers
    // the decompiler lost track of). Each position keeps Ghidra's type when the kind agrees
    // (integer, float, double), and otherwise takes one of the width read. Locked as IMPORTED.
    // An `open` function (it leaves through an indirect tail call, so it may pass registers on
    // unseen) only ever gains parameters.
    private void applyProtos(JsonObject root) throws Exception {
        int set = 0, same = 0, skipped = 0;
        // the call targets no function covered (leaves without .pdata, tools/protos.py): made
        // functions first, so they decompile and take their prototypes below
        if (root.has("leaves")) {
            // (a leaf a pointer reaches is an entry point even inside a body Ghidra drew over it,
            // a callback it absorbed through a jump: made there too, and the body it was taken
            // from fixed up. A leaf only called from inside a function's body is the anti-tamper
            // junk's call into the middle of code, and is left alone)
            java.util.Set<Address> pointed = new java.util.HashSet<>();
            if (root.has("pointed")) {
                for (JsonElement l : root.getAsJsonArray("pointed")) pointed.add(addr(l.getAsString()));
            }
            int made = 0, split = 0;
            for (JsonElement l : root.getAsJsonArray("leaves")) {
                Address a = addr(l.getAsString());
                if (getFunctionAt(a) != null || partOfAnother.contains(a)) continue;
                Function in = getFunctionContaining(a);
                if (in != null && !pointed.contains(a)) continue;
                if (getInstructionAt(a) == null) disassemble(a);
                if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) {
                    made++;
                    if (in != null) {
                        split++;
                        CreateFunctionCmd.fixupFunctionBody(currentProgram, in, monitor);
                    }
                }
            }
            println("ApplyFacts: leaf functions created " + made + " of " + root.getAsJsonArray("leaves").size()
                    + " (" + split + " inside another function's body, " + pointed.size() + " reached by a pointer)");
        }
        // the code after a run of the clean exe's nops: Ghidra's disassembly stopped at the end of
        // the run, leaving the rest of the function undefined bytes, and the decompiler went on
        // through them alone, taking the tail call it found there for code of its own
        // (FUN_141832090 ran into FUN_1418a72a0, its loop's registers `in_R9`, `unaff_RDI`)
        int resumed = 0, rounds = 0;
        boolean more = true;
        while (more && rounds++ < 12) {
            more = false;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isThunk() || f.isExternal()) continue;
                boolean grew = false;
                for (ghidra.program.model.address.AddressRange r : f.getBody()) {
                    Instruction last = getInstructionContaining(r.getMaxAddress());
                    if (last == null || !last.getFlowType().hasFallthrough()) continue;
                    Address next = last.getMaxAddress().add(1);
                    if (currentProgram.getListing().getUndefinedDataAt(next) == null) continue;
                    if (getFunctionAt(next) != null) continue;
                    // (the disassembler stops part way along a long run of one instruction, its
                    // guard against decoding data: the run of nops is decoded here without flow,
                    // and the flow resumes at the code after it)
                    Address code = next;
                    int nops = 0;
                    while (nops < 4096 && currentProgram.getMemory().contains(code)
                            && (currentProgram.getMemory().getByte(code) & 0xff) == 0x90
                            && currentProgram.getListing().getUndefinedDataAt(code) != null) {
                        code = code.add(1);
                        nops++;
                    }
                    if (nops > 0) {
                        new DisassembleCommand(new ghidra.program.model.address.AddressSet(next, code.subtract(1)),
                                null, false).applyTo(currentProgram, monitor);
                    }
                    if (currentProgram.getListing().getUndefinedDataAt(code) != null) {
                        new DisassembleCommand(code, null, true).applyTo(currentProgram, monitor);
                    }
                    if (getInstructionAt(next) != null) {
                        grew = true;
                        resumed++;
                    }
                }
                if (grew) {
                    CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
                    more = true;
                }
            }
        }
        println("ApplyFacts: function code resumed after undisassembled bytes " + resumed);
        // each jump from one function to another's start, now that the code holding it is
        // disassembled: a call that returns, the callee a function of its own and the caller's
        // body recomputed. Left a branch, the callee's code read as the caller's (FUN_141832090
        // decompiled FUN_1418a72a0's loop as its own, reading the registers that callee's own
        // caller had set: `in_R9`, `unaff_RDI`)
        if (root.has("tail_jumps")) {
            // first, a jump an earlier session marked and the facts no longer list goes back to a
            // branch: the overrides are saved with the program and nothing else takes them off.
            // A thunk-resolved jump marked once (`je 0x14065f73d`, into a lone `jmp`) stayed a
            // call to a non-function after protos.py stopped listing it, `func_0x00014065f73d()`
            // with no arguments in 41 functions (2026-10-18)
            java.util.Set<Address> listed = new java.util.HashSet<>();
            for (String k : root.getAsJsonObject("tail_jumps").keySet()) listed.add(addr(k));
            int cleared = 0;
            java.util.Set<Function> refit = new java.util.HashSet<>();
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext()) {
                Instruction insn = it.next();
                if (insn.getFlowOverride() == FlowOverride.CALL_RETURN && insn.getFlowType().isJump()
                        && !listed.contains(insn.getAddress())) {
                    insn.setFlowOverride(FlowOverride.NONE);
                    Function c = getFunctionContaining(insn.getAddress());
                    if (c != null) refit.add(c);
                    cleared++;
                }
            }
            for (Function c : refit) CreateFunctionCmd.fixupFunctionBody(currentProgram, c, monitor);
            println("ApplyFacts: tail jumps no longer listed, back to branches " + cleared);
            int marked = 0;
            for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("tail_jumps").entrySet()) {
                Address j = addr(e.getKey());
                Address t = addr(e.getValue().getAsString());
                Function c = getFunctionContaining(j);
                Instruction insn = getInstructionAt(j);
                if (c == null || insn == null || c.getEntryPoint().equals(t)) continue;
                if (insn.getFlowOverride() == FlowOverride.CALL_RETURN) continue;
                if (getFunctionAt(t) == null && !partOfAnother.contains(t)) {
                    if (getInstructionAt(t) == null) disassemble(t);
                    new CreateFunctionCmd(t).applyTo(currentProgram, monitor);
                }
                insn.setFlowOverride(FlowOverride.CALL_RETURN);
                if (c.getBody().contains(t)) {
                    CreateFunctionCmd.fixupFunctionBody(currentProgram, c, monitor);
                }
                marked++;
            }
            println("ApplyFacts: tail jumps marked calls that return " + marked);
        }
        // no function is decompiled into its callers: each is translated on its own, and inlined
        // its body reads its callers' callers' registers (`in_R9`, `unaff_RDI`)
        int inlined = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isInline()) {
                f.setInline(false);
                inlined++;
            }
        }
        println("ApplyFacts: functions no longer inlined into their callers " + inlined);
        // a function Ghidra made a thunk whose code does more than jump (`mov dl, 1; jmp
        // FUN_14216a080`): decompiled as the target, its callers passed the target's second
        // parameter themselves and read `in_DL` (FUN_141ef4090, 2026-10-16). A function of its own
        int unthunked = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.isThunk() || f.isExternal()) continue;
            Instruction first = getInstructionAt(f.getEntryPoint());
            if (first == null || first.getFlowType().isJump()) continue;
            f.setThunkedFunction(null);
            unthunked++;
        }
        println("ApplyFacts: thunks that do more than jump made functions " + unthunked);
        // functions that do nothing (spills and a ret: the release build's log calls) were given
        // the cspec's security_check_cookie fixup on 2026-10-16, which is no code: but no code
        // also leaves the call's return-address push in place, so the stack drifted 8 bytes a call
        // and the decompiler lost it in 10 of the 85 callers (FUN_142586960: `auStackY_1e8`, the
        // pushes stored as `*(puVar17 + -8) = 0x142586e03`, its second loop behind a 32-byte
        // value; 2026-10-17). They get a calling convention that keeps every register instead
        // (applyNoops); the fixup comes off
        if (root.has("noops")) {
            int cleared = 0;
            for (JsonElement n : root.getAsJsonArray("noops")) {
                Function f = getFunctionAt(addr(n.getAsString()));
                if (f == null || f.isExternal()) continue;
                if ("security_check_cookie".equals(f.getCallFixup())) {
                    f.setCallFixup(null);
                    cleared++;
                }
            }
            println("ApplyFacts: no-op call fixups taken off functions that do nothing " + cleared);
        }
        // each `call __chkstk` of a large frame, a 5-byte nop in Ghidra's copy (tools/protos.py)
        if (root.has("chkstk_calls")) {
            int nopped = 0, done = 0, missing = 0;
            byte[] nop5 = new byte[] { 0x0f, 0x1f, 0x44, 0x00, 0x00 };
            for (JsonElement c : root.getAsJsonArray("chkstk_calls")) {
                Address at = addr(c.getAsString());
                Instruction insn = getInstructionAt(at);
                if (insn == null) {
                    missing++;
                    continue;
                }
                byte[] code = insn.getBytes();
                if (code.length == 5 && (code[0] & 0xff) == 0x0f && (code[1] & 0xff) == 0x1f) {
                    done++;
                    continue;
                }
                if (code.length != 5 || (code[0] & 0xff) != 0xe8) {
                    missing++;
                    continue;
                }
                clearListing(at, at.add(4));
                currentProgram.getMemory().setBytes(at, nop5);
                new DisassembleCommand(at, null, false).applyTo(currentProgram, monitor);
                nopped++;
            }
            println("ApplyFacts: __chkstk calls made nops " + nopped + " (already " + done + "), not a call " + missing);
        }
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("functions").entrySet()) {
            Function f = getFunctionAt(addr(e.getKey()));
            // (a variadic function this script marked takes its fixed parameters from here too)
            Integer fixed = f != null ? vaFixed.get(f.getEntryPoint()) : null;
            if (f == null || f.isThunk() || f.isExternal() || (f.hasVarArgs() && fixed == null)
                    || f.getSignatureSource() == SourceType.USER_DEFINED) {
                skipped++;
                continue;
            }
            JsonObject o = e.getValue().getAsJsonObject();
            JsonArray want = o.getAsJsonArray("params");
            boolean open = o.has("open") && o.get("open").getAsBoolean();
            Parameter[] cur = f.getParameters();
            int n = fixed != null ? fixed : open ? Math.max(want.size(), cur.length) : want.size();
            // a 16-byte value in an xmm register (a vector argument or result): Win64 as Ghidra
            // models it has none, and passes and returns 16 bytes through memory, so these are
            // placed by hand (custom storage). Returned through a hidden pointer, the sincos
            // FUN_14240ad20's two lanes were `extraout_XMM0_Db` in 17 callers, and the hidden
            // pointer moved every parameter of FUN_141345190 one register on (2026-10-16)
            boolean custom = o.has("ret") && "v".equals(o.getAsJsonArray("ret").get(0).getAsString());
            for (int i = 0; i < Math.min(n, want.size()); i++) {
                custom |= "v".equals(want.get(i).getAsJsonArray().get(0).getAsString());
            }
            List<Variable> params = new ArrayList<>();
            boolean differs = n != cur.length || custom != f.hasCustomVariableStorage();
            for (int i = 0; i < n; i++) {
                String kind = i < want.size() ? want.get(i).getAsJsonArray().get(0).getAsString() : "?";
                int size = i < want.size() ? want.get(i).getAsJsonArray().get(1).getAsInt() : 8;
                DataType have = i < cur.length ? cur[i].getDataType() : null;
                DataType dt = paramType(kind, size, have);
                if ("i".equals(kind) && intParams.getOrDefault(f.getEntryPoint(), Set.of()).contains(i + 1)) {
                    dt = concreteInt(size);
                }
                if (have == null || !dt.isEquivalent(have)) differs = true;
                String name = i < cur.length ? cur[i].getName() : "param_" + (i + 1);
                if (name.equals("this")) name = "param_" + (i + 1);
                params.add(custom ? new ParameterImpl(name, dt, paramStorage(i, kind, dt), currentProgram)
                        : new ParameterImpl(name, dt, currentProgram));
            }
            // the result, as wide as the direct callers read it (`test al, al`: a byte). Only
            // functions with direct callers have one: what only a vtable reaches keeps Ghidra's
            // with no result fact (no caller reads it, the function writes nothing of rax) it is
            // void: kept as an earlier pass typed it, a static initializer that registers an atexit
            // handler returned its stack pointer (`BADSPACEBASE`, 106 stubs, 2026-10-16)
            ReturnParameterImpl ret = null;
            DataType have = f.getReturnType();
            DataType dt = o.has("ret")
                    ? returnType(o.getAsJsonArray("ret").get(0).getAsString(), o.getAsJsonArray("ret").get(1).getAsInt(), have)
                    : (open ? have : ghidra.program.model.data.VoidDataType.dataType);
            if (!dt.isEquivalent(have)) {
                differs = true;
                ret = new ReturnParameterImpl(dt, currentProgram);
            }
            if (custom) {
                String rk = o.has("ret") ? o.getAsJsonArray("ret").get(0).getAsString() : null;
                ret = new ReturnParameterImpl(dt, returnStorage(rk, dt), currentProgram);
            }
            // `__thiscall` adds a `this` typed by the class (FID's library signatures): on x64 it
            // is the same registers as __fastcall, and `this` an ordinary first parameter
            String cc = "__thiscall".equals(f.getCallingConventionName()) ? "__fastcall" : null;
            if (cc != null) differs = true;
            if (!differs && f.getSignatureSource() == SourceType.IMPORTED) {
                same++;
                continue;
            }
            f.updateFunction(cc, ret != null ? ret : f.getReturn(), params,
                    custom ? FunctionUpdateType.CUSTOM_STORAGE : FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS,
                    true, SourceType.IMPORTED);
            if (fixed != null) f.setVarArgs(true);
            f.setSignatureSource(SourceType.IMPORTED);
            set++;
            if (custom) customs++;
        }
        println("ApplyFacts: prototypes committed " + set + " (" + customs + " with xmm vectors placed by hand), already so "
                + same + ", skipped " + skipped);
        java.util.Set<Address> noops = new java.util.HashSet<>();
        if (root.has("noops")) for (JsonElement e : root.getAsJsonArray("noops")) noops.add(addr(e.getAsString()));
        java.util.Set<Address> keepXmm0 = new java.util.HashSet<>();
        if (root.has("keep_xmm0")) for (JsonElement e : root.getAsJsonArray("keep_xmm0")) keepXmm0.add(addr(e.getAsString()));
        applyKeepConventions(keepXmm0, noops);
    }

    // the volatile integer registers each function preserves (tools/preserved.py), read in run()
    private final Map<Address, java.util.List<String>> preserved = new HashMap<>();
    private boolean preservedChosen;

    // Each function's calling convention from what it leaves as it found it: everything (a no-op),
    // xmm0 (protos.py keep_xmm0), and the volatile integer registers (preserved.py), one
    // convention per combination, __fastcall_keep_r8_r9_xmm0 and so on. A function that has one of
    // these and no longer qualifies goes back to __fastcall; any other convention is left alone.
    private void applyKeepConventions(java.util.Set<Address> keepXmm0, java.util.Set<Address> noops) throws Exception {
        Map<Address, String> want = new HashMap<>();
        java.util.Set<Address> every = new java.util.HashSet<>(preservedChosen ? java.util.Collections.emptySet() : keepXmm0);
        every.addAll(noops);
        every.addAll(preserved.keySet());
        for (Address a : every) {
            if (noops.contains(a)) {
                want.put(a, KEEP_ALL);
                continue;
            }
            java.util.List<String> regs = preserved.getOrDefault(a, java.util.Collections.emptyList());
            boolean x = preservedChosen ? regs.contains("XMM0") : keepXmm0.contains(a);
            if (preservedChosen && regs.size() == 1 && x) {
                want.put(a, KEEP_XMM0);
                continue;
            }
            if (regs.isEmpty()) {
                want.put(a, KEEP_XMM0);
                continue;
            }
            StringBuilder n = new StringBuilder("__fastcall_keep");
            for (String r : KEEPABLE) if (regs.contains(r)) n.append('_').append(r.toLowerCase());
            if (x) n.append("_xmm0");
            want.put(a, n.toString());
        }
        // the conventions, each added (or replaced) once
        Class<?> se = Function.class.getClassLoader().loadClass("ghidra.program.database.SpecExtension");
        Object ext = se.getConstructor(ghidra.program.model.listing.Program.class).newInstance(currentProgram);
        java.lang.reflect.Method add = se.getMethod("addReplaceCompilerSpecExtension", String.class,
                ghidra.util.task.TaskMonitor.class);
        for (String name : new java.util.TreeSet<>(want.values())) add.invoke(ext, conventionXml(name), monitor);
        reclaimConventionSlots(want);
        int set = 0, kept = 0, reset = 0, fell = 0;
        // The program holds only so many convention names (a byte's worth, and a name once used
        // keeps its slot): with xmm1-xmm5 there were 355 kinds, and 285 of them failed "allocation
        // capacity exceeded" (2026-10-18). The commonest go first; a function whose own does not
        // fit gets the working one that keeps the most of its registers and nothing more (a
        // register it does not keep is only killed by the call, as under __fastcall)
        Map<String, Integer> uses = new HashMap<>();
        for (String w : want.values()) uses.merge(w, 1, Integer::sum);
        java.util.List<Function> order = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) order.add(f);
        order.sort((x, y) -> Integer.compare(uses.getOrDefault(want.get(y.getEntryPoint()), 0),
                uses.getOrDefault(want.get(x.getEntryPoint()), 0)));
        java.util.Set<String> working = new java.util.HashSet<>(), failed = new java.util.HashSet<>();
        for (Function f : order) {
            String cur = f.getCallingConventionName();
            boolean ours = cur != null && cur.startsWith("__fastcall_keep");
            String w = want.get(f.getEntryPoint());
            if (w == null) {
                if (ours) {
                    f.setCallingConvention("__fastcall");
                    reset++;
                }
                continue;
            }
            if (f.isExternal() || f.isThunk() || f.hasCustomVariableStorage()) continue;
            if (w.equals(cur)) {
                working.add(w);
                kept++;
                continue;
            }
            // (only over __fastcall or one of these: a function given another convention keeps it)
            if (cur != null && !cur.equals("__fastcall") && !ours && !cur.equals("unknown") && !cur.equals("default")) continue;
            String c = failed.contains(w) ? fallback(w, working) : w;
            while (true) {
                try {
                    f.setCallingConvention(c);
                } catch (Exception e) {
                    // (a refused name is reported as an error and the convention left as it was)
                }
                if (c.equals(f.getCallingConventionName()) || c.equals("__fastcall")) break;
                failed.add(c);
                working.remove(c);
                c = fallback(w, working);
            }
            working.add(c);
            if (!c.equals(w)) fell++;
            set++;
        }
        println("ApplyFacts: keep conventions (" + new java.util.TreeSet<>(want.values()).size() + " kinds, "
                + failed.size() + " beyond the program's capacity) given to " + set + " functions ("
                + fell + " with a narrower one; already " + kept + "), " + reset + " back to __fastcall");
    }

    // The program's table of convention names holds 126 (a signed byte's ids) and never lets one go:
    // every keep convention an earlier session assigned kept its slot after nothing used it, and on
    // 2026-10-18 one slot was left, so 33 of the 49 conventions tools/preserved.py chose could not
    // be named and 5,488 functions fell back to narrower ones. Before assigning: every function
    // whose keep convention is about to change goes to __fastcall, and each keep name no function
    // and no function definition then uses is deleted from the table (its record, through the
    // adapter the data type manager keeps), freeing its id. Anything unexpected and it is left as
    // it was: the assignment below still falls back to what fits.
    private void reclaimConventionSlots(Map<Address, String> want) {
        try {
            int moved = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                String cur = f.getCallingConventionName();
                if (cur != null && cur.startsWith("__fastcall_keep") && !cur.equals(want.get(f.getEntryPoint()))) {
                    f.setCallingConvention("__fastcall");
                    moved++;
                }
            }
            java.util.Set<String> used = new java.util.HashSet<>();
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.getCallingConventionName() != null) used.add(f.getCallingConventionName());
            }
            ghidra.program.model.data.DataTypeManager dtm = currentProgram.getDataTypeManager();
            java.util.Iterator<ghidra.program.model.data.DataType> all = dtm.getAllDataTypes();
            while (all.hasNext()) {
                ghidra.program.model.data.DataType d = all.next();
                if (d instanceof ghidra.program.model.data.FunctionDefinition fd && fd.getCallingConventionName() != null) {
                    used.add(fd.getCallingConventionName());
                }
            }
            Class<?> c = dtm.getClass();
            while (c != null && !c.getSimpleName().equals("DataTypeManagerDB")) c = c.getSuperclass();
            java.lang.reflect.Field af = c.getDeclaredField("callingConventionAdapter");
            af.setAccessible(true);
            Object adapter = af.get(dtm);
            java.lang.reflect.Method names = adapter.getClass().getDeclaredMethod("getCallingConventionNames");
            names.setAccessible(true);
            names.invoke(adapter);      // (fills the adapter's name -> id map)
            java.lang.reflect.Field mf = adapter.getClass().getDeclaredField("callingConventionNameToIDMap");
            mf.setAccessible(true);
            @SuppressWarnings("unchecked")
            Map<String, Byte> ids = new HashMap<>((Map<String, Byte>) mf.get(adapter));
            java.lang.reflect.Field tf = adapter.getClass().getDeclaredField("callingConventionTable");
            tf.setAccessible(true);
            // (through reflection: the script bundle's manifest does not import the db package, as it
            // did not import ghidra.program.model.pcode, see applyCallsites)
            Object table = tf.get(adapter);
            // (keyed by a byte field, not a long: "Field key required")
            Class<?> fieldClass = table.getClass().getClassLoader().loadClass("db.Field");
            Class<?> byteField = table.getClass().getClassLoader().loadClass("db.ByteField");
            java.lang.reflect.Constructor<?> key = byteField.getConstructor(byte.class);
            java.lang.reflect.Method delete = table.getClass().getMethod("deleteRecord", fieldClass);
            int freed = 0;
            for (Map.Entry<String, Byte> e : ids.entrySet()) {
                if (e.getKey().startsWith("__fastcall_keep") && !used.contains(e.getKey())) {
                    if ((Boolean) delete.invoke(table, key.newInstance(e.getValue()))) freed++;
                }
            }
            java.lang.reflect.Method inv = adapter.getClass().getDeclaredMethod("invalidateCache");
            inv.setAccessible(true);
            inv.invoke(adapter);
            dtm.getClass().getMethod("invalidateCache").invoke(dtm);
            println("ApplyFacts: convention slots: " + moved + " functions moved off a changing convention, "
                    + freed + " unused names freed of " + ids.size());
        } catch (Throwable t) {
            println("ApplyFacts: convention slots not reclaimed (" + t + "); assigning within what is left");
        }
    }

    // the registers a keep convention leaves alone, from its name
    private static java.util.Set<String> keptBy(String name) {
        java.util.Set<String> out = new java.util.HashSet<>();
        if (KEEP_ALL.equals(name)) {
            out.addAll(java.util.Arrays.asList(KEEPABLE));
            out.add("XMM0");
            out.add("RAX");
            return out;
        }
        if (KEEP_XMM0.equals(name)) {
            out.add("XMM0");
            return out;
        }
        if (name.startsWith("__fastcall_keep_")) {
            for (String t : name.substring("__fastcall_keep_".length()).split("_")) out.add(t.toUpperCase());
        }
        return out;
    }

    // the working convention that keeps the most of what `want` keeps and nothing else, or __fastcall
    private static String fallback(String want, java.util.Set<String> working) {
        java.util.Set<String> regs = keptBy(want);
        String best = "__fastcall";
        int size = 0;
        for (String w : working) {
            java.util.Set<String> k = keptBy(w);
            if (regs.containsAll(k) && (k.size() > size || (k.size() == size && w.compareTo(best) < 0))) {
                best = w;
                size = k.size();
            }
        }
        return best;
    }

    private int customs;

    // The functions xmm0 comes out of as it went in (tools/protos.py: none of their instructions
    // writes it and all they call are such functions). The whole-program build keeps a value in
    // xmm0 across a call to one; under Win64's __fastcall, where xmm0 is killed by any call, the
    // decompiler printed the value after the call `extraout_XMM0` (FUN_140315680). A calling
    // convention of their own, __fastcall with xmm0 among the unaffected registers, added to the
    // program as a compiler-spec extension
    private static final String KEEP_XMM0 = "__fastcall_keepxmm0";
    // and the functions that do nothing at all (tools/protos.py `noops`): __fastcall with every
    // register unaffected, which is what they do, so a value kept in rax or a volatile register
    // across a call to one stays the same variable (not `extraout_EAX`)
    private static final String KEEP_ALL = "__fastcall_keepall";

    // the volatile integer registers a convention keeps beside xmm0 (tools/preserved.py), as named
    // (and xmm1-xmm5, as preserved.py finds them since 2026-10-18)
    private static final String[] KEEPABLE = { "RAX", "RCX", "RDX", "R8", "R9", "R10", "R11",
            "XMM1", "XMM2", "XMM3", "XMM4", "XMM5" };

    private static String conventionXml(String name) {
        // __fastcall_keepxmm0, __fastcall_keepall, or __fastcall_keep_<regs>[_xmm0]
        boolean all = KEEP_ALL.equals(name);
        boolean xmm0 = KEEP_XMM0.equals(name) || name.endsWith("_xmm0");
        java.util.Set<String> regs = new java.util.HashSet<>();
        if (name.startsWith("__fastcall_keep_")) {
            for (String r : KEEPABLE) {
                if (java.util.Arrays.asList(name.substring("__fastcall_keep_".length()).split("_")).contains(r.toLowerCase())) {
                    regs.add(r);
                }
            }
        }
        String g = "<group><pentry minsize=\"4\" maxsize=\"8\" metatype=\"float\"><register name=\"XMM%d_Qa\"/></pentry>"
                + "<pentry minsize=\"1\" maxsize=\"8\"><register name=\"%s\"/></pentry></group>";
        StringBuilder x = new StringBuilder();
        x.append("<prototype name=\"").append(name).append("\" extrapop=\"8\" stackshift=\"8\">");
        x.append("<input pointermax=\"8\">");
        String[] gpr = { "RCX", "RDX", "R8", "R9" };
        for (int i = 0; i < 4; i++) x.append(String.format(g, i, gpr[i]));
        x.append("<pentry minsize=\"1\" maxsize=\"500\" align=\"8\"><addr offset=\"40\" space=\"stack\"/></pentry>");
        x.append("<rule><datatype name=\"struct\" sizes=\"3,5,6,7\"/><convert_to_ptr/></rule></input>");
        x.append("<output><pentry minsize=\"4\" maxsize=\"8\" metatype=\"float\"><register name=\"XMM0_Qa\"/></pentry>");
        x.append("<pentry minsize=\"1\" maxsize=\"8\"><register name=\"RAX\"/></pentry>");
        x.append("<rule><datatype name=\"struct\" sizes=\"3,5,6,7\"/><hidden_return/></rule></output>");
        x.append("<unaffected><varnode space=\"ram\" offset=\"0\" size=\"8\"/>");
        for (String r : new String[] { "RBX", "RBP", "RDI", "RSI", "RSP", "R12", "R13", "R14", "R15", "DF", "GS_OFFSET",
                "XMM6", "XMM7", "XMM8", "XMM9", "XMM10", "XMM11", "XMM12", "XMM13", "XMM14", "XMM15" }) {
            x.append("<register name=\"").append(r).append("\"/>");
        }
        if (xmm0 || all) x.append("<register name=\"XMM0\"/>");
        if (!all) {
            for (String r : regs) x.append("<register name=\"").append(r).append("\"/>");
        }
        if (all) {
            for (String r : new String[] { "RAX", "RCX", "RDX", "R8", "R9", "R10", "R11",
                    "XMM1", "XMM2", "XMM3", "XMM4", "XMM5" }) {
                x.append("<register name=\"").append(r).append("\"/>");
            }
            x.append("</unaffected>");
        } else {
            // every volatile register but xmm0, as __fastcall kills them. With only RAX listed,
            // rcx, rdx, r8-r11 and xmm1-5 were neither killed nor kept, and the decompiler let a
            // value flow through a call in them: `FUN_1405ee730(in_R8)` in FUN_1405ef2d0, which
            // passes rcx on untouched, and in_R8/in_R9/in_R10/in_R11 read around calls to the
            // 12,471 functions that have this convention (2026-10-18)
            // (and no <killedbycall> at all when it would be empty: a convention that keeps every
            // volatile register, rax among them since 2026-10-18, failed the spec's schema with an
            // empty one, "uncompleted content model")
            StringBuilder killed = new StringBuilder();
            for (String r : new String[] { "RAX", "RCX", "RDX", "R8", "R9", "R10", "R11",
                    "XMM1", "XMM2", "XMM3", "XMM4", "XMM5" }) {
                if (!regs.contains(r)) killed.append("<register name=\"").append(r).append("\"/>");
            }
            if (!xmm0) killed.append("<register name=\"XMM0\"/>");
            x.append("</unaffected>");
            if (killed.length() > 0) x.append("<killedbycall>").append(killed).append("</killedbycall>");
        }
        x.append("<localrange><range space=\"stack\" first=\"0xfffffffffff0bdc1\" last=\"0xffffffffffffffff\"/>");
        x.append("<range space=\"stack\" first=\"8\" last=\"39\"/></localrange></prototype>");
        return x.toString();
    }

    // the convention `name` (a compiler-spec extension, added once) on the listed functions but
    // `skip`, and __fastcall back on any that has it and is no longer listed
    private void applyConvention(String name, JsonArray list, java.util.Set<Address> skip) throws Exception {
        {
            // (every session, add or replace: added only when missing, a corrected definition
            // never reached a program that had the old one)
            // (through Ghidra's own class loader, as JumpTable: the script bundle does not import
            // ghidra.program.database)
            Class<?> se = Function.class.getClassLoader().loadClass("ghidra.program.database.SpecExtension");
            Object ext = se.getConstructor(ghidra.program.model.listing.Program.class).newInstance(currentProgram);
            se.getMethod("addReplaceCompilerSpecExtension", String.class, ghidra.util.task.TaskMonitor.class)
                    .invoke(ext, conventionXml(name), monitor);
        }
        int set = 0, kept = 0, reset = 0;
        java.util.Set<Address> want = new java.util.HashSet<>();
        for (JsonElement e : list) want.add(addr(e.getAsString()));
        want.removeAll(skip);
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (name.equals(f.getCallingConventionName()) && !want.contains(f.getEntryPoint())) {
                f.setCallingConvention("__fastcall");
                reset++;
            }
        }
        if (reset > 0) println("ApplyFacts: " + reset + " functions no longer listed for " + name + ": __fastcall");
        for (Address a : want) {
            Function f = getFunctionAt(a);
            if (f == null || f.isExternal() || f.isThunk() || f.hasCustomVariableStorage()) continue;
            if (name.equals(f.getCallingConventionName())) {
                kept++;
                continue;
            }
            f.setCallingConvention(name);
            set++;
        }
        println("ApplyFacts: " + name + " given to " + set + " functions (already " + kept + ")");
    }

    // switches.json (tools/switches.py): jump tables read off the instructions, written as a
    // jump-table override where Ghidra has no flow out of the jump (the decompiler then followed
    // it into nothing: FUN_1406303d0, "Flow exceeded maximum allowable instructions"). A function
    // Ghidra made at a case is removed: it is the switch's own code
    private void applySwitches(JsonObject root) throws Exception {
        int set = 0, resolved = 0, missing = 0, removed = 0;
        Class<?> jt = Function.class.getClassLoader().loadClass("ghidra.program.model.pcode.JumpTable");
        // (Ghidra 12: JumpTable(Address, ArrayList<Address>, boolean override, int format); older
        // versions take the first three)
        java.lang.reflect.Constructor<?> make = null;
        for (java.lang.reflect.Constructor<?> c : jt.getConstructors()) {
            Class<?>[] ps = c.getParameterTypes();
            if ((ps.length == 3 || (ps.length == 4 && ps[3] == int.class)) && ps[0] == Address.class
                    && ps[1].isAssignableFrom(ArrayList.class) && ps[2] == boolean.class) {
                make = c;
            }
        }
        if (make == null) {
            StringBuilder have = new StringBuilder();
            for (java.lang.reflect.Constructor<?> c : jt.getConstructors()) have.append(c).append("; ");
            println("ApplyFacts: no JumpTable(Address, List, boolean) constructor: " + have);
            return;
        }
        java.lang.reflect.Method write = jt.getMethod("writeOverride", Function.class);
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("jumps").entrySet()) {
            Address j = addr(e.getKey());
            Instruction insn = getInstructionAt(j);
            Function f = getFunctionContaining(j);
            if (insn == null || f == null) {
                missing++;
                continue;
            }
            boolean flows = false;
            for (ghidra.program.model.symbol.Reference r : insn.getReferencesFrom()) {
                if (r.getReferenceType().isJump() || r.getReferenceType().isComputed()) flows = true;
            }
            // (and where Ghidra's table is shorter than the one the instructions give: it truncated
            // it, "Sanity check requires truncation of jumptable", and FUN_1419c3450 ran on into
            // the bytes after it as code)
            // (or a different one: FUN_1419c3450's two-level table, a case of Ghidra's own running
            // into data, "Bad instruction - Truncating control flow here")
            java.util.Set<Address> have = new java.util.HashSet<>();
            for (ghidra.program.model.symbol.Reference r : insn.getReferencesFrom()) {
                if (r.getReferenceType().isJump() || r.getReferenceType().isComputed()) have.add(r.getToAddress());
            }
            for (Address fl : insn.getFlows()) have.add(fl);
            java.util.Set<Address> ours = new java.util.HashSet<>();
            for (JsonElement t : e.getValue().getAsJsonArray()) ours.add(addr(t.getAsString()));
            if (have.equals(ours)) {
                resolved++;
                continue;
            }
            // (Ghidra's references that are not cases go: they are what the decompiler followed)
            for (ghidra.program.model.symbol.Reference r : insn.getReferencesFrom()) {
                if ((r.getReferenceType().isJump() || r.getReferenceType().isComputed()) && !ours.contains(r.getToAddress())) {
                    currentProgram.getReferenceManager().delete(r);
                }
            }
            ArrayList<Address> dests = new ArrayList<>();
            for (JsonElement t : e.getValue().getAsJsonArray()) {
                Address d = addr(t.getAsString());
                Function at = getFunctionAt(d);
                if (at != null && !at.equals(f)) {
                    currentProgram.getFunctionManager().removeFunction(d);
                    removed++;
                }
                if (getInstructionAt(d) == null) disassemble(d);
                currentProgram.getReferenceManager().addMemoryReference(j, d,
                        ghidra.program.model.symbol.RefType.COMPUTED_JUMP, SourceType.USER_DEFINED, 0);
                dests.add(d);
            }
            Object table = make.getParameterCount() == 4 ? make.newInstance(j, dests, true, 0) : make.newInstance(j, dests, true);
            write.invoke(table, f);
            CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
            set++;
        }
        println("ApplyFacts: jump tables written " + set + " (Ghidra had " + resolved + "), functions at cases removed "
                + removed + ", not found " + missing);
    }

    // Win64's place for position i (0-based) of a kind: rcx/rdx/r8/r9 as wide as the type, the
    // low lane of xmm0-3 for a float or double, all of it for a vector, the stack from entry +0x28
    private ghidra.program.model.listing.VariableStorage paramStorage(int i, String kind, DataType dt) throws Exception {
        if (i >= 4) {
            return new ghidra.program.model.listing.VariableStorage(currentProgram, 0x28 + 8 * (i - 4), dt.getLength());
        }
        String reg;
        switch (kind) {
            case "f": case "x": reg = "XMM" + i + "_Da"; break;
            case "d": reg = "XMM" + i + "_Qa"; break;
            case "v": reg = "XMM" + i; break;
            default: {
                String[][] names = { { "CL", "CX", "ECX", "RCX" }, { "DL", "DX", "EDX", "RDX" },
                        { "R8B", "R8W", "R8D", "R8" }, { "R9B", "R9W", "R9D", "R9" } };
                int w = dt.getLength();
                reg = names[i][w <= 1 ? 0 : w <= 2 ? 1 : w <= 4 ? 2 : 3];
            }
        }
        return new ghidra.program.model.listing.VariableStorage(currentProgram, currentProgram.getRegister(reg));
    }

    private ghidra.program.model.listing.VariableStorage returnStorage(String kind, DataType dt) throws Exception {
        if (kind == null || dt instanceof ghidra.program.model.data.VoidDataType) {
            return ghidra.program.model.listing.VariableStorage.VOID_STORAGE;
        }
        String reg;
        switch (kind) {
            case "f": case "x": reg = "XMM0_Da"; break;
            case "d": reg = "XMM0_Qa"; break;
            case "v": reg = "XMM0"; break;
            default: {
                int w = dt.getLength();
                reg = w <= 1 ? "AL" : w <= 2 ? "AX" : w <= 4 ? "EAX" : "RAX";
            }
        }
        return new ghidra.program.model.listing.VariableStorage(currentProgram, currentProgram.getRegister(reg));
    }

    // callsites.json (tools/protos.py): each indirect call's arguments as the caller sets them up,
    // written as a call-site signature override. Without one the decompiler guesses from the
    // registers it sees set and drops what it cannot tie to the call: cross_ssl's start lost the
    // logger in r8 at its factory Initialize call (0x141f2ee98), and the SChannel setup then logged
    // through a garbage pointer. One signature per distinct shape, shared by every site.
    private void applyCallsites(JsonObject root) throws Exception {
        Map<String, FunctionDefinitionDataType> shapes = new HashMap<>();
        int written = 0, missing = 0, failed = 0;
        // HighFunctionDBUtil through Ghidra's own class loader: the script bundle's manifest
        // did not import its package (ghidra.program.model.pcode), and the class was not found
        // however it was named, aborting the session before its exports (2026-10-16)
        java.lang.reflect.Method writeOverride = Function.class.getClassLoader()
                .loadClass("ghidra.program.model.pcode.HighFunctionDBUtil")
                .getMethod("writeOverride", Function.class, Address.class,
                        ghidra.program.model.listing.FunctionSignature.class);
        String firstError = null;
        // every call-site override from earlier sessions goes first: writeOverride adds a symbol
        // per signature (`override::prt_1417dbac9_<hash>`) and removes none, so a call kept every
        // override it was ever given and the decompiler went on using an old one (the float
        // return written for 0x1417dbac9 beside two undefined8 ones, 2026-10-16)
        List<ghidra.program.model.symbol.Symbol> stale = new ArrayList<>();
        for (ghidra.program.model.symbol.Symbol s : currentProgram.getSymbolTable().getAllSymbols(false)) {
            Namespace p = s.getParentNamespace();
            if (p != null && "override".equals(p.getName()) && s.getName().startsWith("prt_")) stale.add(s);
        }
        for (ghidra.program.model.symbol.Symbol s : stale) s.delete();
        println("ApplyFacts: earlier call-site overrides removed " + stale.size());
        // an indirect tail jump (`jmp [rax + 0x48]`) is a call that returns: the decompiler then
        // shows it as one and takes its call-site signature, which it ignores on a jump. HMAC's
        // update wrapper FUN_1420bd740 measures its string into r8 and jumps on through a table;
        // as a jump its call showed no arguments and the length was lost (2026-10-20)
        int jumpsMarked = 0;
        if (root.has("jumps")) {
            java.util.Set<Function> refit = new java.util.HashSet<>();
            for (JsonElement j : root.getAsJsonArray("jumps")) {
                Instruction insn = getInstructionAt(addr(j.getAsString()));
                if (insn == null || !insn.getFlowType().isJump()) continue;
                if (insn.getFlowOverride() != FlowOverride.CALL_RETURN) {
                    insn.setFlowOverride(FlowOverride.CALL_RETURN);
                    Function c = getFunctionContaining(insn.getAddress());
                    if (c != null) refit.add(c);
                }
                jumpsMarked++;
            }
            for (Function c : refit) CreateFunctionCmd.fixupFunctionBody(currentProgram, c, monitor);
        }
        println("ApplyFacts: indirect tail jumps made calls that return " + jumpsMarked);
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("calls").entrySet()) {
            Address at = addr(e.getKey());
            Function f = getFunctionContaining(at);
            if (f == null) {
                missing++;
                continue;
            }
            JsonArray args = e.getValue().getAsJsonArray();
            StringBuilder key = new StringBuilder();
            for (JsonElement a : args) {
                JsonArray p = a.getAsJsonArray();
                key.append(p.get(0).getAsString()).append(p.get(1).getAsInt());
            }
            // a direct call (to a variadic function, tools/protos.py) returns what its callee
            // does; an indirect one a register's worth
            DataType ret = Undefined.getUndefinedDataType(8);
            // an indirect call's result as its caller reads it (a float through `call [rax +
            // 0x68]` was `extraout_XMM0_Da` against undefined8: 46 stubs, 2026-10-16)
            JsonObject rets = root.has("rets") ? root.getAsJsonObject("rets") : null;
            if (rets != null && rets.has(e.getKey())) {
                JsonArray r = rets.getAsJsonArray(e.getKey());
                ret = returnType(r.get(0).getAsString(), r.get(1).getAsInt(), null);
                key.append("_r").append(r.get(0).getAsString()).append(r.get(1).getAsInt());
            }
            Instruction insn = getInstructionAt(at);
            Address[] flows = insn != null ? insn.getFlows() : null;
            // (a call through a pointer whose target Ghidra knows, the CFG guard's `call [rip +
            // __guard_dispatch_icall_fptr]`, has a flow to that routine: what the caller reads
            // of the real target's result is the return, not the guard routine's void)
            if (flows != null && flows.length == 1 && (rets == null || !rets.has(e.getKey()))) {
                Function callee = getFunctionAt(flows[0]);
                if (callee != null) {
                    ret = callee.getReturnType();
                    key.append("_r").append(ret.getName().replaceAll("\\W", ""));
                }
            }
            FunctionDefinitionDataType sig = shapes.get(key.toString());
            if (sig == null) {
                sig = new FunctionDefinitionDataType("icall_" + (key.length() == 0 ? "void" : key.toString()));
                List<ghidra.program.model.data.ParameterDefinition> defs = new ArrayList<>();
                int i = 0;
                for (JsonElement a : args) {
                    JsonArray p = a.getAsJsonArray();
                    defs.add(new ghidra.program.model.data.ParameterDefinitionImpl("param_" + (++i),
                            paramType(p.get(0).getAsString(), p.get(1).getAsInt(), null), null));
                }
                sig.setArguments(defs.toArray(new ghidra.program.model.data.ParameterDefinition[0]));
                sig.setReturnType(ret);
                shapes.put(key.toString(), sig);
            }
            try {
                writeOverride.invoke(null, f, at, sig);
                written++;
            } catch (Exception ex) {
                if (firstError == null) {
                    Throwable cause = ex.getCause() != null ? ex.getCause() : ex;
                    firstError = at + ": " + cause;
                }
                failed++;
            }
        }
        println("ApplyFacts: indirect call signatures written " + written + " (" + shapes.size() + " shapes), "
                + "outside any function " + missing + ", failed " + failed
                + (firstError != null ? " (first: " + firstError + ")" : ""));
    }

    // a result's type: Ghidra's when it is as wide as the callers read and of the same kind, else
    // one of that width (a byte read of an undefined8 result was `in_RAX & 0xffffffffffffff00`)
    private DataType returnType(String kind, int size, DataType have) {
        boolean haveFloat = have instanceof ghidra.program.model.data.AbstractFloatDataType;
        switch (kind) {
            case "f":
                return haveFloat && have.getLength() == 4 ? have : ghidra.program.model.data.FloatDataType.dataType;
            case "d":
                return haveFloat && have.getLength() == 8 ? have : ghidra.program.model.data.DoubleDataType.dataType;
            case "x":
                return haveFloat ? have : ghidra.program.model.data.FloatDataType.dataType;
            case "v":
                // a vector result (xmm0 whole: a sincos's two lanes): 16 bytes
                return new ArrayDataType(Undefined.getUndefinedDataType(1), 16, 1);
            default:
                if (have != null && !haveFloat && have.getLength() == size && plain(have)) return have;
                return Undefined.getUndefinedDataType(Math.min(8, Math.max(1, size)));
        }
    }

    // intparams.json (tools/intparams.py): integer positions the decompiler typed float of its
    // own accord, from a spill or a whole-register move, though the code reads them as integers.
    // An undefined type left it free to do so again; these get a concrete unsigned one. The
    // UnitData reader FUN_1425019e0's flags came out `float param_4`, tested as `(uint)param_4 &
    // 1`, a conversion: the owner-only GUID was skipped, every later field read 2 bytes early, and
    // the player's class was 0 (2026-10-21)
    private final Map<Address, Set<Integer>> intParams = new HashMap<>();

    // and the home slot a pinned register parameter is spilled to (entry rsp + 8 * position, the
    // first four): typed float there too, `local_res20[0] = (float)param_4;` and every test of the
    // flags went through `(uint)local_res20[0]`, a conversion again (FUN_1425019e0, 2026-10-21)
    private void applyIntParamHomes(JsonObject functions) {
        int made = 0;
        for (Map.Entry<Address, Set<Integer>> e : intParams.entrySet()) {
            Function f = getFunctionAt(e.getKey());
            String key = "0x" + Long.toHexString(e.getKey().getOffset());
            JsonObject o = functions.has(key) ? functions.getAsJsonObject(key) : null;
            if (f == null || o == null) continue;
            JsonArray want = o.getAsJsonArray("params");
            ghidra.program.model.listing.StackFrame sf = f.getStackFrame();
            for (int pos : e.getValue()) {
                if (pos > 4 || pos > want.size()) continue;
                int size = want.get(pos - 1).getAsJsonArray().get(1).getAsInt();
                // (none in the frame yet: the decompiler makes its own and types it from the
                // moves, so one is made; an integer one already there is left alone)
                Variable v = sf.getVariableContaining(8 * pos);
                if (v != null && v.getStackOffset() != 8 * pos) {
                    println("ApplyFacts: home slot of " + f.getName() + " param " + pos + " inside " + v.getName()
                            + " at " + v.getStackOffset() + ", left");
                    continue;
                }
                String name = v != null ? v.getName() : String.format("local_res%x", 8 * pos);
                // (the whole 8-byte slot: a 4-byte variable there was ignored, the decompiler saw
                // the slot accessed whole and made its own `float local_res20 [2]` over it)
                DataType slot = size >= 8 ? concreteInt(8) : new ArrayDataType(concreteInt(size), 8 / Math.max(1, size), size);
                if (v != null && v.getDataType().isEquivalent(slot)) continue;
                try {
                    if (v != null) sf.clearVariable(8 * pos);
                    sf.createVariable(name, 8 * pos, slot, SourceType.USER_DEFINED);
                    made++;
                    println("ApplyFacts: home slot of " + f.getName() + " param " + pos + " (" + name + ") "
                            + (v != null ? "retyped from " + v.getDataType().getName() : "made") + " " + slot.getName());
                } catch (Exception ex) {
                    println("ApplyFacts: home slot of " + f.getName() + " param " + pos + " failed: " + ex.getMessage());
                }
            }
        }
        println("ApplyFacts: integer parameters' home slots retyped " + made);
    }

    private static DataType concreteInt(int size) {
        switch (Math.min(8, Math.max(1, size))) {
            case 1: return ghidra.program.model.data.ByteDataType.dataType;
            case 2: return ghidra.program.model.data.UnsignedShortDataType.dataType;
            case 4: return ghidra.program.model.data.UnsignedIntegerDataType.dataType;
            default: return ghidra.program.model.data.UnsignedLongLongDataType.dataType;
        }
    }

    // a position's type: Ghidra's when it is of the kind the instructions read, else one of the
    // width read ("-": nothing reads it, but a later position is used, so it is still passed)
    private DataType paramType(String kind, int size, DataType have) {
        boolean haveFloat = have instanceof ghidra.program.model.data.AbstractFloatDataType;
        switch (kind) {
            case "f":
                return haveFloat && have.getLength() == 4 ? have : ghidra.program.model.data.FloatDataType.dataType;
            case "d":
                return haveFloat && have.getLength() == 8 ? have : ghidra.program.model.data.DoubleDataType.dataType;
            case "x":
                // an xmm argument only ever moved whole: float or double is not in the code
                return haveFloat ? have : ghidra.program.model.data.FloatDataType.dataType;
            case "v":
                // four lanes in the register (packed arithmetic reads them): 16 bytes
                return new ArrayDataType(Undefined.getUndefinedDataType(1), 16, 1);
            case "i":
                // Ghidra's only when it is as wide as the code reads: an int kept for a register
                // read whole made the decompiler fill the upper half with a register it lost
                // track of (`CONCAT44(in_register_00000014, param_2)`), 591 stubs on 2026-10-16
                // Nor wider than it: a uint kept for a byte the code reads (`movzx edi, r8b`)
                // made every caller that sets only r8b pass `CONCAT31((int3)(in_R8D >> 8), 1)`
                // And only a type C knows: a class or array pointer (`MRECmpImpl *this`, an FID
                // library signature's; `undefined1 (*param_1) [16]`, Ghidra's for a pointer SSE
                // reads through) left `->` and type names the translator cannot spell (~40
                // stubs, 2026-10-16)
                if (have != null && !haveFloat && have.getLength() == Math.min(8, size) && plain(have)) {
                    return have;
                }
                // (one stored as a float: an undefined type of its width would let the decompiler
                // infer the float again, see intParams)
                if (haveFloat) return concreteInt(size);
                return Undefined.getUndefinedDataType(Math.min(8, Math.max(1, size)));
            default:
                if (have != null && have.getLength() > 0 && have.getLength() <= 8 && plain(have)) return have;
                return Undefined.getUndefinedDataType(8);
        }
    }

    // Every function name a call can be read back by: a name more than one function has (FID's
    // `FID_conflict:fallbackMethod` on three, library copies the image links twice) gets its
    // address appended, so a decompiled call says which one (tools/translate.py resolves a
    // `name_<address>` to that address; by the name alone 20 calls did not link, 2026-10-16)
    private void uniqueNames() throws Exception {
        Map<String, List<Function>> byName = new HashMap<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (f.isThunk() || f.isExternal() || f.getName().startsWith("FUN_")) continue;
            byName.computeIfAbsent(f.getName(true), k -> new ArrayList<>()).add(f);
        }
        int renamed = 0;
        for (List<Function> fs : byName.values()) {
            if (fs.size() < 2) continue;
            for (Function f : fs) {
                String suffix = "_" + f.getEntryPoint().toString();
                if (f.getName().endsWith(suffix)) continue;
                f.setName(f.getName().replace(':', '_') + suffix, SourceType.IMPORTED);
                renamed++;
            }
        }
        println("ApplyFacts: functions sharing a name made unique by address " + renamed);
    }

    // globalptrs.json: .data globals the code dereferences after loading them whole. Typed
    // `undefined *` where nothing typed them (no data, or an undefined of up to 8 bytes): the
    // decompiler then casts each access through one to its width, where it printed `*DAT_x` of a
    // bare undefined8 that the translator cannot spell (13 stubs, 2026-10-16)
    private void applyGlobalPtrs(JsonObject root) throws Exception {
        int set = 0, typed = 0, inside = 0;
        DataType bytePtr = new PointerDataType(Undefined.getUndefinedDataType(1));
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("globals").entrySet()) {
            Address g = addr(e.getKey());
            Data d = getDataAt(g);
            if (d == null) {
                Data over = currentProgram.getListing().getDataContaining(g);
                if (over != null) {
                    inside++;
                    continue;
                }
                if (currentProgram.getListing().getInstructionContaining(g) != null) {
                    inside++;
                    continue;
                }
            } else {
                DataType t = d.getDataType();
                boolean untyped = t instanceof ghidra.program.model.data.Undefined
                        || t instanceof ghidra.program.model.data.DefaultDataType;
                if (!untyped || d.getLength() > 8) {
                    typed++;
                    continue;
                }
            }
            // (and nothing typed starts inside the 8 bytes it takes)
            boolean clash = false;
            for (int k = 1; k < 8 && !clash; k++) {
                Data dk = getDataAt(g.add(k));
                clash = dk != null && !(dk.getDataType() instanceof ghidra.program.model.data.Undefined
                        || dk.getDataType() instanceof ghidra.program.model.data.DefaultDataType);
            }
            if (clash) {
                inside++;
                continue;
            }
            clearListing(g, g.add(7));
            createData(g, bytePtr);
            set++;
        }
        println("ApplyFacts: global pointers typed " + set + ", already typed " + typed + ", inside other data " + inside);
    }

    // an integer, or a pointer to one (or to nothing, or to such a pointer): what the decompiled
    // C can spell without the program's own types
    private static boolean plain(DataType t) {
        while (t instanceof ghidra.program.model.data.TypeDef) {
            t = ((ghidra.program.model.data.TypeDef) t).getBaseDataType();
        }
        if (t instanceof Pointer) {
            DataType to = ((Pointer) t).getDataType();
            return to == null || to instanceof ghidra.program.model.data.VoidDataType || plain(to)
                    || to instanceof ghidra.program.model.data.AbstractFloatDataType;
        }
        return t instanceof ghidra.program.model.data.AbstractIntegerDataType
                || t instanceof ghidra.program.model.data.Undefined
                || t instanceof ghidra.program.model.data.DefaultDataType
                || t instanceof ghidra.program.model.data.BooleanDataType
                || t instanceof ghidra.program.model.data.CharDataType;
    }

    // retcheck.json: functions carrying the anti-tamper return-address check.
    private void applyRetcheck(JsonObject root) throws Exception {
        int stale = dropStaleTags("aegis:retcheck", root.getAsJsonObject("functions").keySet());
        if (stale > 0) println("ApplyFacts: dropped " + stale + " stale aegis:retcheck tags");
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("functions").entrySet()) {
            Function f = getFunctionAt(addr(e.getKey()));
            if (f == null) continue;
            f.addTag("aegis:retcheck");
            Address tail = addr(e.getValue().getAsString());
            setEOLComment(tail, "anti-tamper return-address check starts here: protection, not game logic");
            tagged++;
        }
        if (root.has("junk_jumps")) applyJunkJumps(root.getAsJsonObject("junk_jumps"),
                root.has("junk_ends") ? root.getAsJsonObject("junk_ends") : new JsonObject());
        if (root.has("checks")) applyChecks(root.getAsJsonObject("checks"));
    }

    // A jump an earlier session made unconditional that tools/retcheck.py no longer lists: its
    // bytes back as the file has them, and the code after it decoded again with flow. Patched,
    // it took the program's bytes with it, and the junk cleared after it was real code: the
    // `je` past Lua::SendChatMessage's lua_isstring test went straight to the usage error, the
    // 40 KB of the handler after it left undefined bytes, and the function was stubbed as one
    // the decompiler lost calls in (2026-10-18). Only that shape is undone, an original
    // conditional jump (`0f 8x`, `7x`) now `e9`/`eb`: the skipped checks and the __chkstk nops are
    // patches of other bytes and stay.
    private void revertStaleJunkJumps(JsonObject jumps) throws Exception {
        java.util.Set<Address> current = new java.util.HashSet<>();
        for (String k : jumps.keySet()) current.add(addr(k));
        ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
        ghidra.program.model.mem.MemoryBlock text = mem.getBlock(".text");
        if (text == null) return;
        int reverted = 0, regrown = 0;
        java.util.Set<Function> touched = new java.util.HashSet<>();
        for (ghidra.program.model.mem.MemoryBlockSourceInfo info : text.getSourceInfos()) {
            if (info.getFileBytes().isEmpty()) continue;
            // (through reflection: the script bundle does not import the db package, as for the
            // convention table in reclaimConventionSlots)
            Object fb = info.getFileBytes().get();
            java.lang.reflect.Method original = fb.getClass().getMethod("getOriginalBytes", long.class, byte[].class);
            Address lo = info.getMinAddress();
            long size = info.getLength();
            int chunk = 1 << 20;
            for (long off = 0; off < size; off += chunk) {
                int n = (int) Math.min(chunk + 8, size - off);
                byte[] now = new byte[n], orig = new byte[n];
                mem.getBytes(lo.add(off), now);
                original.invoke(fb, info.getFileBytesOffset() + off, orig);
                for (int i = 0; i < Math.min(chunk, n); i++) {
                    if (now[i] == orig[i]) continue;
                    int o0 = orig[i] & 0xff, n0 = now[i] & 0xff;
                    int len;
                    if (o0 == 0x0f && i + 1 < n && (orig[i + 1] & 0xf0) == 0x80 && n0 == 0xe9) len = 6;
                    else if ((o0 & 0xf0) == 0x70 && n0 == 0xeb) len = 2;
                    else continue;
                    Address j = lo.add(off + i);
                    i += len - 1;
                    if (current.contains(j)) continue;
                    Function f = getFunctionContaining(j);
                    // (code only where code runs into it: a jump table's entry taken for a jump,
                    // FUN_1417354c0's `0x73`, gets its byte back and stays data)
                    Instruction prev = getInstructionContaining(j.subtract(1));
                    boolean code = prev != null && prev.getMaxAddress().equals(j.subtract(1)) && prev.getFlowType().hasFallthrough();
                    clearListing(j, j.add(len - 1));
                    byte[] back = java.util.Arrays.copyOfRange(orig, (int) (j.subtract(lo) - off), (int) (j.subtract(lo) - off) + len);
                    mem.setBytes(j, back);
                    reverted++;
                    if (!code) continue;
                    DisassembleCommand cmd = new DisassembleCommand(j, null, true);
                    cmd.applyTo(currentProgram, monitor);
                    Instruction insn = getInstructionAt(j);
                    if (insn != null) {
                        insn.setFlowOverride(FlowOverride.NONE);
                        // (and its fall-through, which the old patch had cleared as junk)
                        Address ft = insn.getFallThrough();
                        if (ft != null && getInstructionAt(ft) == null) {
                            new DisassembleCommand(ft, null, true).applyTo(currentProgram, monitor);
                            if (getInstructionAt(ft) != null) regrown++;
                        }
                    }
                    if (f != null) touched.add(f);
                }
            }
        }
        for (Function f : touched) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        println("ApplyFacts: stale jumps over junk reverted " + reverted + " (" + regrown
                + " fall-throughs decoded again), in " + touched.size() + " functions");
    }

    // Each check skipped (tools/retcheck.py): its start, the 7-byte load of the range base,
    // becomes `jmp T; nop; nop`, T being where the function's code resumes once the return
    // address passes. The check is then unreachable and the decompiler leaves it out; stripped
    // from the decompiled text instead, 143 functions in shapes the pattern did not know were
    // stubbed (2026-10-16). Ghidra's copy only, as for the junk jumps; done already, left alone.
    private void applyChecks(JsonObject checks) throws Exception {
        int set = 0, done = 0, missing = 0;
        for (Map.Entry<String, JsonElement> e : checks.entrySet()) {
            Address s = addr(e.getKey());
            Address t = addr(e.getValue().getAsString());
            Instruction insn = getInstructionAt(s);
            // (Ghidra's decode out of step at the start, or none there at all: a check that a jump
            // over junk lands on, FUN_1424db710's at 0x1424dc570 among six left unpatched,
            // 2026-10-19. The bytes say what is there: the 7-byte `mov reg, [rip + base]`, every code
            // unit across them cleared, then decoded alone)
            if (insn == null || insn.getLength() != 7) {
                byte[] b7 = new byte[7];
                currentProgram.getMemory().getBytes(s, b7);
                boolean load = ((b7[0] & 0xfb) == 0x48) && (b7[1] & 0xff) == 0x8b && (b7[2] & 0xc7) == 0x05;
                if (load) {
                    Address end = s.add(6);
                    CodeUnit first = currentProgram.getListing().getCodeUnitContaining(s);
                    CodeUnit last = currentProgram.getListing().getCodeUnitContaining(end);
                    Address lo = first != null && first.getMinAddress().compareTo(s) < 0 ? first.getMinAddress() : s;
                    Address hi = last != null && last.getMaxAddress().compareTo(end) > 0 ? last.getMaxAddress() : end;
                    clearListing(lo, hi);
                    new DisassembleCommand(s, null, false).applyTo(currentProgram, monitor);
                    insn = getInstructionAt(s);
                }
            }
            if (insn == null) {
                missing++;
                continue;
            }
            byte[] code = insn.getBytes();
            if ((code[0] & 0xff) == 0xe9) {
                done++;
                clearDeadCheck(s, t);
                continue;
            }
            if (code.length != 7) {
                missing++;
                continue;
            }
            long rel = t.getOffset() - (s.getOffset() + 5);
            byte[] patched = new byte[] { (byte) 0xe9, (byte) rel, (byte) (rel >> 8), (byte) (rel >> 16),
                    (byte) (rel >> 24), (byte) 0x90, (byte) 0x90 };
            clearListing(s, s.add(6));
            currentProgram.getMemory().setBytes(s, patched);
            new DisassembleCommand(s, null, false).applyTo(currentProgram, monitor);
            if (getInstructionAt(s) == null) {
                missing++;
                continue;
            }
            if (getInstructionAt(t) == null) {
                disassemble(t);
            }
            clearDeadCheck(s, t);
            set++;
        }
        for (Function f : deadCheckFunctions) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        println("ApplyFacts: return-address checks skipped " + set + " (already " + done + "), not patched " + missing
                + "; dead check bodies cleared " + deadCleared + ", kept for a jump into them " + deadKept);
    }

    private int deadCleared, deadKept;
    private final java.util.Set<Function> deadCheckFunctions = new java.util.HashSet<>();

    // A skipped check's own body, from past its start's jump to where the code resumes: dead once
    // the start jumps over it, but still in the listing, and the decompiler decodes every block of
    // a function before it drops the unreachable ones, so the junk in the check's crash path
    // marked the decompilation damaged (CreateFrame's core, FUN_1424db710, warned of an overlap at
    // 0x1424dc1ea in its second check and was kept as protection junk, 2026-10-19). Cleared only
    // when it runs forward to the resumption and nothing outside it jumps in.
    private void clearDeadCheck(Address s, Address t) throws Exception {
        Address lo = s.add(7);
        if (t.compareTo(lo) <= 0 || t.subtract(lo) > 0x4000) return;
        Address hi = t.subtract(1);
        ghidra.program.model.address.AddressSet region = new ghidra.program.model.address.AddressSet(lo, hi);
        ghidra.program.model.symbol.ReferenceManager rm = currentProgram.getReferenceManager();
        ghidra.program.model.address.AddressIterator dests = rm.getReferenceDestinationIterator(region, true);
        while (dests.hasNext()) {
            Address d = dests.next();
            for (ghidra.program.model.symbol.Reference r : rm.getReferencesTo(d)) {
                Address from = r.getFromAddress();
                if (from.compareTo(s) < 0 || from.compareTo(hi) > 0) {
                    deadKept++;
                    return;
                }
            }
        }
        Function f = getFunctionContaining(s);
        CodeUnit across = currentProgram.getListing().getCodeUnitContaining(hi);
        if (across != null && across.getMaxAddress().compareTo(hi) > 0) hi = across.getMinAddress().subtract(1);
        if (hi.compareTo(lo) < 0) return;
        clearListing(lo, hi);
        if (f != null) deadCheckFunctions.add(f);
        deadCleared++;
    }

    // The check's conditional jumps over junk (tools/retcheck.py): the fall-through bytes are no
    // code, and decoded they run straight across the jump's own target, so the decompiler saw
    // overlapping instructions and lost the function (155 stubs, 2026-10-16). Each jump is made
    // unconditional (a flow override), the junk is cleared, and the target disassembled.
    private void applyJunkJumps(JsonObject jumps, JsonObject ends) throws Exception {
        revertStaleJunkJumps(jumps);
        int set = 0, missing = 0;
        for (Map.Entry<String, JsonElement> e : jumps.entrySet()) {
            Address j = addr(e.getKey());
            Address t = addr(e.getValue().getAsString());
            Instruction insn = getInstructionAt(j);
            if (insn == null) {
                // Ghidra's own decode went through the junk and is out of step here: whatever
                // covers the jump goes, and the jump is decoded where it starts
                CodeUnit over = currentProgram.getListing().getCodeUnitContaining(j);
                if (over != null) {
                    clearListing(over.getMinAddress(), over.getMaxAddress());
                }
                CodeUnit next = currentProgram.getListing().getCodeUnitContaining(j.add(1));
                if (next != null && next.getMinAddress().compareTo(j) > 0) {
                    clearListing(next.getMinAddress(), next.getMaxAddress());
                }
                // the jump alone, without following its flow back into the junk
                new DisassembleCommand(j, null, false).applyTo(currentProgram, monitor);
                insn = getInstructionAt(j);
            }
            if (insn == null) {
                missing++;
                continue;
            }
            // The jump made unconditional in the program's bytes: a flow override does not
            // reach a conditional jump (the decompiler still took the fall-through into the junk
            // and reported the overlap, 2026-10-16). `0f 8x rel32` becomes `e9 rel32+1; nop` and
            // `7x rel8` becomes `eb rel8`, to the same target. Ghidra's copy only: the exe on
            // disk, which the Python tools read, keeps its bytes
            byte[] code = insn.getBytes();
            byte[] patched = null;
            if (code.length == 6 && (code[0] & 0xff) == 0x0f && (code[1] & 0xf0) == 0x80) {
                int rel = (code[2] & 0xff) | (code[3] & 0xff) << 8 | (code[4] & 0xff) << 16 | (code[5] & 0xff) << 24;
                rel += 1;
                patched = new byte[] { (byte) 0xe9, (byte) rel, (byte) (rel >> 8), (byte) (rel >> 16),
                        (byte) (rel >> 24), (byte) 0x90 };
            } else if (code.length == 2 && (code[0] & 0xf0) == 0x70) {
                patched = new byte[] { (byte) 0xeb, code[1] };
            }
            if (patched != null) {
                clearListing(j, j.add(code.length - 1));
                currentProgram.getMemory().setBytes(j, patched);
                new DisassembleCommand(j, null, false).applyTo(currentProgram, monitor);
                insn = getInstructionAt(j);
                if (insn == null) {
                    missing++;
                    continue;
                }
            }
            Address after = j.add(insn.getLength());
            // the junk, and whatever decoded instruction spans its end: up to the target, or
            // (tools/retcheck.py junk_ends) up to the real code the function reaches before it,
            // the block an opposite jump just before goes to (`jl X; jge T` with X between)
            Address stop = ends.has(e.getKey()) ? addr(ends.get(e.getKey()).getAsString()) : t;
            Address end = stop.subtract(1);
            CodeUnit across = currentProgram.getListing().getCodeUnitContaining(stop);
            if (across != null && across.getMinAddress().compareTo(stop) < 0) {
                end = across.getMaxAddress();
            }
            if (after.compareTo(end) <= 0) {
                clearListing(after, end);
            }
            insn.setFlowOverride(FlowOverride.BRANCH);
            if (getInstructionAt(t) == null) {
                disassemble(t);
            }
            if (!stop.equals(t) && getInstructionAt(stop) == null) {
                disassemble(stop);
            }
            set++;
        }
        println("ApplyFacts: jumps over junk made unconditional " + set + ", not an instruction " + missing);
    }

    // Functions that are part of another (tools/pieces.py): a chained .pdata piece, or an entry
    // strictly inside a .pdata function that only its parent jumps to. Each split cost both
    // halves their registers. Removed every session (RemovePieceFunctions.java did it once;
    // analysis made new ones), DEFAULT-named only, and the parents re-flow into the code.
    private void applyPieces(JsonObject root) throws Exception {
        java.util.Set<Address> parents = new java.util.TreeSet<>();
        int removed = 0, failed = 0;
        for (String key : new String[] {"pieces", "interior"}) {
            JsonArray list = root.getAsJsonArray(key);
            if (list == null) continue;
            for (JsonElement el : list) {
                Address a = addr(el.getAsString());
                partOfAnother.add(a);
                Function f = getFunctionAt(a);
                // (and analysis-named: a switch case Ghidra made a function of,
                // `switchD_1416f346c::caseD_6`, is a piece all the same; only user and imported
                // names are kept)
                if (f == null || (f.getSymbol().getSource() != SourceType.DEFAULT
                        && f.getSymbol().getSource() != SourceType.ANALYSIS)) continue;
                try {
                    // (a ConcurrentModificationException inside Ghidra's removal is state-
                    // dependent and clears in a later session: logged, not fatal)
                    removeFunction(f);
                    removed++;
                    Function parent = getFunctionBefore(a);
                    if (parent != null) parents.add(parent.getEntryPoint());
                } catch (java.util.ConcurrentModificationException ex) {
                    failed++;
                }
            }
        }
        for (Address p : parents) {
            Function f = getFunctionAt(p);
            if (f != null) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        }
        println("ApplyFacts: removed " + removed + " functions on pieces and interior entries ("
                + failed + " removals threw), re-flowed " + parents.size() + " parents");
    }

    private void applySrcrefs(JsonObject root) throws Exception {
        int stale = dropStaleTags("src:", root.getAsJsonObject("functions").keySet());
        if (stale > 0) println("ApplyFacts: dropped " + stale + " stale src: tags");
        for (Map.Entry<String, JsonElement> e : root.getAsJsonObject("functions").entrySet()) {
            // tag existing functions only: creating one here is how 1,787 functions ended
            // up on chained .pdata pieces (see RemovePieceFunctions.java)
            Function f = getFunctionAt(addr(e.getKey()));
            if (f == null) continue;
            JsonArray files = e.getValue().getAsJsonArray();
            StringBuilder sb = new StringBuilder();
            for (JsonElement fe : files) {
                String path = fe.getAsString();
                String file = path.substring(path.lastIndexOf('/') + 1);
                f.addTag("src:" + file);
                if (sb.length() > 0) sb.append('\n');
                sb.append("src: ").append(path);
            }
            String old = f.getComment();
            if (old == null || !old.contains("src: ")) {
                f.setComment(old == null ? sb.toString() : old + "\n" + sb);
            }
            tagged++;
        }
    }
}
