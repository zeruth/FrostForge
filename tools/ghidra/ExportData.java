// Exports every defined data item of the program, one JSON object per line:
// {"addr","len","type","name","block"}. tools/translate.py declares the reference globals the
// decompiled code names (DAT_..., PTR_..., s_...) with these types and sizes.
//
// Usage (headless): -postScript ExportData.java <outfile.jsonl>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Symbol;

import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportData extends GhidraScript {

    private static String esc(String s) {
        StringBuilder b = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') {
                b.append('\\').append(c);
            } else if (c < 0x20) {
                b.append(String.format("\\u%04x", (int) c));
            } else {
                b.append(c);
            }
        }
        return b.toString();
    }

    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        int count = 0;
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Data d = it.next();
                MemoryBlock block = currentProgram.getMemory().getBlock(d.getAddress());
                Symbol sym = currentProgram.getSymbolTable().getPrimarySymbol(d.getAddress());
                String name = sym != null ? sym.getName(true) : "";
                w.printf("{\"addr\":\"%s\",\"len\":%d,\"type\":\"%s\",\"name\":\"%s\",\"block\":\"%s\"}%n",
                         d.getAddress().toString(), d.getLength(), esc(d.getDataType().getDisplayName()),
                         esc(name), block != null ? esc(block.getName()) : "");
                count++;
            }
        }
        println("ExportData: wrote " + count + " data items to " + out);
    }
}
