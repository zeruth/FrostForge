// The calling-convention names the program's database holds, and how many slots it has left:
// build/facts/convention_names.json. A name once assigned keeps its slot (one byte's worth of
// them), so tools/preserved.py builds the keep conventions out of the names already there first
// and adds new ones only as far as the slots left allow (2026-10-18: 33 of 49 new names did not
// fit, the slots held by earlier sessions' names).
//@category FrostLite

import java.io.File;
import java.io.FileWriter;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;

public class ListConventions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "convention_names.json";
        Object dtm = currentProgram.getDataTypeManager();
        Class<?> c = dtm.getClass();
        while (c != null && !c.getSimpleName().equals("DataTypeManagerDB")) c = c.getSuperclass();
        java.lang.reflect.Field f = c.getDeclaredField("callingConventionAdapter");
        f.setAccessible(true);
        Object adapter = f.get(dtm);
        java.lang.reflect.Method names = adapter.getClass().getDeclaredMethod("getCallingConventionNames");
        names.setAccessible(true);
        @SuppressWarnings("unchecked")
        Set<String> held = new TreeSet<>((Set<String>) names.invoke(adapter));
        StringBuilder s = new StringBuilder("{\"held\": [");
        boolean first = true;
        for (String n : held) {
            s.append(first ? "" : ", ").append('"').append(n.replace("\"", "\\\"")).append('"');
            first = false;
        }
        s.append("], \"count\": ").append(held.size()).append("}\n");
        try (FileWriter w = new FileWriter(new File(out))) {
            w.write(s.toString());
        }
        println("ListConventions: " + held.size() + " names held -> " + out);
    }
}
