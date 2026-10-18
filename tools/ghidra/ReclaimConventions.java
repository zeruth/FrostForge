// Frees the calling-convention name slots no function and no function definition uses (the
// program's table holds 126 and never lets a name go, see ApplyFacts.reclaimConventionSlots).
// Moves no function: only names nothing uses go. Prints each step and, on failure, the causes.
//@category FrostLite

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.DataTypeManager;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.listing.Function;

public class ReclaimConventions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        try {
            Set<String> used = new HashSet<>();
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.getCallingConventionName() != null) used.add(f.getCallingConventionName());
            }
            DataTypeManager dtm = currentProgram.getDataTypeManager();
            java.util.Iterator<DataType> all = dtm.getAllDataTypes();
            while (all.hasNext()) {
                DataType d = all.next();
                if (d instanceof FunctionDefinition fd && fd.getCallingConventionName() != null) {
                    used.add(fd.getCallingConventionName());
                }
            }
            println("ReclaimConventions: " + used.size() + " names in use");
            Class<?> c = dtm.getClass();
            while (c != null && !c.getSimpleName().equals("DataTypeManagerDB")) c = c.getSuperclass();
            java.lang.reflect.Field af = c.getDeclaredField("callingConventionAdapter");
            af.setAccessible(true);
            Object adapter = af.get(dtm);
            println("ReclaimConventions: adapter " + adapter.getClass().getName());
            java.lang.reflect.Method names = adapter.getClass().getDeclaredMethod("getCallingConventionNames");
            names.setAccessible(true);
            names.invoke(adapter);
            java.lang.reflect.Field mf = adapter.getClass().getDeclaredField("callingConventionNameToIDMap");
            mf.setAccessible(true);
            @SuppressWarnings("unchecked")
            Map<String, Byte> ids = new HashMap<>((Map<String, Byte>) mf.get(adapter));
            println("ReclaimConventions: " + ids.size() + " names held");
            java.lang.reflect.Field tf = adapter.getClass().getDeclaredField("callingConventionTable");
            tf.setAccessible(true);
            Object table = tf.get(adapter);
            println("ReclaimConventions: table " + table.getClass().getName());
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
            println("ReclaimConventions: deleted " + freed);
            java.lang.reflect.Method inv = adapter.getClass().getDeclaredMethod("invalidateCache");
            inv.setAccessible(true);
            inv.invoke(adapter);
            println("ReclaimConventions: adapter cache invalidated");
            dtm.getClass().getMethod("invalidateCache").invoke(dtm);
            println("ReclaimConventions: " + freed + " unused names freed of " + ids.size());
        } catch (Throwable t) {
            Throwable x = t;
            while (x != null) {
                println("ReclaimConventions: failed: " + x);
                for (StackTraceElement s : x.getStackTrace()) {
                    if (s.getClassName().startsWith("ghidra") || s.getClassName().startsWith("db")) {
                        println("    at " + s);
                    }
                }
                x = x.getCause();
            }
            throw t;
        }
    }
}
