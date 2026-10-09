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
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import java.io.File;
import java.nio.file.Files;
import java.util.Map;

public class ApplyFacts extends GhidraScript {
    private SymbolTable symbols;
    private int named, kept, created, tagged, labelled;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args.length > 0 ? args[0] : "build/facts");
        symbols = currentProgram.getSymbolTable();

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
        // hand-identified names last, so they win over anything extracted
        File names = new File(dir, "names.json");
        if (names.exists()) applyNames(read(names));

        println(String.format("ApplyFacts: named %d (kept %d user names), created %d functions, " +
                "tagged %d, labelled %d vtables", named, kept, created, tagged, labelled));
    }

    private JsonObject read(File f) throws Exception {
        return JsonParser.parseString(Files.readString(f.toPath())).getAsJsonObject();
    }

    private Address addr(String hex) {
        return toAddr(Long.parseUnsignedLong(hex.startsWith("0x") ? hex.substring(2) : hex, 16));
    }

    private Function functionAt(Address a) {
        Function f = getFunctionAt(a);
        if (f == null) {
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
        f.setParentNamespace(namespace(ns));
        f.setName(name, SourceType.IMPORTED);
        named++;
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

    // Import slots recovered from a live process by tools/iat_dump.py: label each slot
    // with its API name and type it as a pointer, so `call [slot]` reads as the API.
    private void applyImports(JsonObject root) throws Exception {
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
            Symbol existing = getSymbolAt(a);
            if (existing != null && existing.getSource() == SourceType.USER_DEFINED) {
                kept++;
                continue;
            }
            createLabel(a, name, namespace("Import::" + dll.replace('.', '_')),
                    true, SourceType.IMPORTED);
            try {
                clearListing(a, a.add(7));
                createData(a, PointerDataType.dataType);
            } catch (Exception ex) {
                // keep analysis' typing
            }
            named++;
        }
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
