// The program's bytes at an address against the file's original bytes, and every place in .text
// where they differ, by kind (a check of what earlier sessions patched).
//   bash tools/ghidra/ghidra.sh script ProbeBytes.java 0x1417357dc 0xa8
// @category FrostLite
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.mem.MemoryBlockSourceInfo;

public class ProbeBytes extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address at = toAddr(Long.parseUnsignedLong(args[0].replace("0x", ""), 16));
        int len = Integer.decode(args[1]);
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(at);
        for (MemoryBlockSourceInfo info : text.getSourceInfos()) {
            if (info.getFileBytes().isEmpty() || !info.contains(at)) continue;
            Object fb = info.getFileBytes().get();
            java.lang.reflect.Method original = fb.getClass().getMethod("getOriginalBytes", long.class, byte[].class);
            byte[] now = new byte[len], orig = new byte[len];
            mem.getBytes(at, now);
            original.invoke(fb, info.getFileBytesOffset() + at.subtract(info.getMinAddress()), orig);
            StringBuilder a = new StringBuilder(), b = new StringBuilder();
            for (int i = 0; i < len; i++) {
                a.append(String.format("%02x", now[i]));
                b.append(String.format("%02x", orig[i]));
            }
            println("PROBE now  " + a);
            println("PROBE file " + b);
            // and every difference in the block, grouped by the first byte now / in the file
            java.util.Map<String, Integer> kinds = new java.util.TreeMap<>();
            long size = info.getLength();
            Address lo = info.getMinAddress();
            int chunk = 1 << 20, sites = 0;
            for (long off = 0; off < size; off += chunk) {
                int n = (int) Math.min(chunk, size - off);
                byte[] x = new byte[n], y = new byte[n];
                mem.getBytes(lo.add(off), x);
                original.invoke(fb, info.getFileBytesOffset() + off, y);
                for (int i = 0; i < n; i++) {
                    if (x[i] == y[i]) continue;
                    String k = String.format("%02x<-%02x", x[i], y[i]);
                    kinds.merge(k, 1, Integer::sum);
                    if (sites++ < 0) break;
                    while (i + 1 < n && x[i + 1] != y[i + 1]) i++;
                }
            }
            println("PROBE differing runs " + sites + " by first byte (now<-file) " + kinds);
        }
    }
}
