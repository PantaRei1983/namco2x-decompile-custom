// SeedTables.java -- Ghidra headless script: find the code a 68020 program only reaches through its DISPATCH TABLES.
//
// These games call their mode / task / object routines as  jsr ([bd,PC,Xn.w*4])  (and jmp): a table of 32-bit routine addresses
// right in the code, indexed by a state word. Ghidra's analysis does not follow them (and decodes that addressing mode as
// post-indexed: tools/gen/lift68k.py fix_preindexed), so everything behind a table is never disassembled.
//
// Each round: every such instruction's table is read -- entries while they are even addresses inside the image that hold a
// plausible first opcode -- each entry is disassembled and made a function, then the auto-analysis runs again (new code has new
// tables). Also takes a seeds file (one hex address a line: addresses a RUN trapped on, reached through tables in RAM or
// address registers) as the script argument. Stops when a round adds nothing.
// @category Namco22
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

public class SeedTables extends GhidraScript {
    private Memory mem;
    private long size;

    private boolean plausible(long t) throws Exception {
        if (t < 0x400 || t >= size || (t & 1) != 0) return false;
        int w = mem.getShort(toAddr(t)) & 0xFFFF;
        return w != 0x0000 && w != 0xFFFF;
    }

    // An instruction nothing leads to: no instruction falls through into it and no branch or call names it. Such a one is the
    // start of a run that came from a table entry or a sweep -- the weaker evidence when a direct branch disagrees with it.
    private boolean weak(Instruction v) {
        Instruction b = v.getPrevious();
        if (b != null && v.getAddress().equals(b.getFallThrough())) return false;
        for (ghidra.program.model.symbol.Reference r : getReferencesTo(v.getAddress()))
            if (r.getReferenceType().isFlow() && !r.getReferenceType().isComputed()) return false;   // Ghidra's computed ones are guesses (it reads these tables wrongly)
        return true;
    }

    // DOES IT READ AS CODE? Instructions decoded one after another from a, falling through, must reach an instruction that ends a
    // routine (rts, jmp, bra ...) or run into known code, with nothing undecodable, no 0000 0000 (padding / data) and no
    // instruction already there at a different alignment. (Ghidra's own isValidSubroutine turns down plain routines.)
    private boolean reads(Address a) {
        try {
            for (int k = 0; k < 800; k++) {
                if (a.getOffset() + 2 > size) return false;
                Instruction have = currentProgram.getListing().getInstructionContaining(a);
                if (have != null) return have.getAddress().equals(a);
                if ((mem.getInt(a) == 0) || (mem.getShort(a) & 0xFFFF) == 0xFFFF) return false;
                ghidra.app.util.PseudoInstruction pi = pseudo.disassemble(a);
                if (pi == null) return false;
                Address ft = pi.getFallThrough();
                if (ft == null) return true;
                a = ft;
            }
        } catch (Exception e) { }
        return false;
    }

    // DISASSEMBLE AT a, CLEARING WHAT IS IN THE WAY. A wrong guess (a table read one entry too far, say) leaves instructions that
    // start in the middle of the real ones, and the real ones then cannot be made. always = a is known to be code (a run got there);
    // otherwise only instructions that nothing leads to are cleared.
    private boolean carve(Address a, boolean always) throws Exception {
        for (int tries = 0; tries < 8; tries++) {
            if (currentProgram.getListing().getInstructionAt(a) != null) return true;
            Instruction v = currentProgram.getListing().getInstructionContaining(a);
            if (v == null) {
                new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                if (currentProgram.getListing().getInstructionAt(a) != null) return true;
                for (int k = 1; k < 10 && v == null; k++) v = currentProgram.getListing().getInstructionAt(a.add(k));
                ghidra.program.model.listing.Data d = currentProgram.getListing().getDefinedDataContaining(a);
                if (d == null) for (int k = 1; k < 10 && d == null; k++) d = currentProgram.getListing().getDefinedDataAt(a.add(k));
                if (d != null) { currentProgram.getListing().clearCodeUnits(d.getMinAddress(), d.getMaxAddress(), false); continue; }
            }
            if (v == null) return false;
            if (!always && !weak(v)) return false;
            ghidra.program.model.listing.Function f = currentProgram.getFunctionManager().getFunctionAt(v.getAddress());
            if (f != null) removeFunction(f);
            currentProgram.getListing().clearCodeUnits(v.getMinAddress(), v.getMaxAddress(), false);
            carved++;
        }
        return currentProgram.getListing().getInstructionAt(a) != null;
    }
    private int carved;
    private ghidra.app.util.PseudoDisassembler pseudo;

    // the SWITCH tables found (16-bit offsets after a jmp): data, never code -- the sweep, the repair and the seeds keep out of them
    private final java.util.TreeMap<Long, Long> switchTables = new java.util.TreeMap<>();
    private boolean inTable(long t) { java.util.Map.Entry<Long, Long> e = switchTables.floorEntry(t); return e != null && t <= e.getValue(); }
    private boolean seed(long t) throws Exception { return seed(t, false); }
    private boolean seed(long t, boolean always) throws Exception {
        if (inTable(t)) return false;
        Address a = toAddr(t);
        boolean added = false;
        if (currentProgram.getListing().getInstructionAt(a) == null) {
            if (!always && !reads(a)) return false;                    // does not read as a routine: not code
            if (!carve(a, always)) return false;
            added = true;
        }
        if (currentProgram.getListing().getInstructionAt(a) != null && currentProgram.getFunctionManager().getFunctionAt(a) == null) {
            try { createFunction(a, null); added = true; } catch (Exception e) { }
        }
        return added;
    }

    // CODE CUT SHORT BY DATA. Ghidra's reference analysers mark bytes that look like addresses as data (the 00E0xxxx operand of a
    // move, say) before the code around them is known; a routine disassembled later then stops dead at that data. Every instruction
    // whose fall-through or branch target is inside the image but is no instruction: clear whatever data sits there, disassemble on.
    private int repair() throws Exception {
        int fixed = 0;
        for (int pass = 0; pass < 50; pass++) {
            java.util.ArrayList<Long> todo = new java.util.ArrayList<>();
            InstructionIterator ii = currentProgram.getListing().getInstructions(true);
            while (ii.hasNext()) {
                Instruction in = ii.next();
                Address ft = in.getFallThrough();
                if (ft != null && currentProgram.getListing().getInstructionAt(ft) == null) todo.add(ft.getOffset());
                for (Address f : in.getFlows())
                    if (f.getOffset() < size && currentProgram.getListing().getInstructionAt(f) == null) todo.add(f.getOffset());
            }
            int n = 0;
            for (long t : todo) {
                if (t < 0x400 || t >= size || (t & 1) != 0 || inTable(t)) continue;
                Address a = toAddr(t);
                if (currentProgram.getListing().getInstructionAt(a) != null) continue;
                if (currentProgram.getListing().getInstructionContaining(a) != null) { if (carve(a, false)) n++; continue; }
                long end = Math.min(size - 1, t + 15);
                ghidra.program.model.listing.Data d = currentProgram.getListing().getDefinedDataContaining(a);
                if (d != null) end = Math.max(end, d.getMaxAddress().getOffset());
                // clear only DATA in the way, never instructions
                for (long x = t; x <= end; ) {
                    ghidra.program.model.listing.Data dd = currentProgram.getListing().getDefinedDataContaining(toAddr(x));
                    if (dd != null) { currentProgram.getListing().clearCodeUnits(dd.getMinAddress(), dd.getMaxAddress(), false); x = dd.getMaxAddress().getOffset() + 1; }
                    else x++;
                }
                if (carve(a, false)) n++;
            }
            fixed += n;
            if (n == 0) break;
        }
        if (fixed > 0) println("SeedTables: repaired " + fixed + " places where code ran into data");
        return fixed;
    }

    // ROUTINES NOTHING NAMES. Routines reached only through tables built in RAM or through address registers have no reference at
    // all, but they sit where code sits: straight after the rts / jmp / bra that ends the routine before. Every such gap that reads
    // as a routine (PseudoDisassembler: valid instructions to a proper end) is taken as one.
    private int sweep() throws Exception {
        java.util.ArrayList<Long> todo = new java.util.ArrayList<>();
        InstructionIterator ii = currentProgram.getListing().getInstructions(true);
        while (ii.hasNext()) {
            Instruction in = ii.next();
            if (in.getFallThrough() != null) continue;
            long g = in.getMaxAddress().getOffset() + 1;
            if (!plausible(g) || inTable(g)) continue;
            Address a = toAddr(g);
            if (currentProgram.getListing().getInstructionContaining(a) != null) continue;
            if (currentProgram.getListing().getDefinedDataContaining(a) != null) continue;
            todo.add(g);
        }
        int n = 0;
        for (long g : todo) {
            Address a = toAddr(g);
            if (currentProgram.getListing().getInstructionContaining(a) != null) continue;
            if (!reads(a)) continue;
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            if (currentProgram.getListing().getInstructionAt(a) == null) continue;
            if (currentProgram.getFunctionManager().getFunctionContaining(a) == null) try { createFunction(a, null); } catch (Exception e) { }
            n++;
        }
        if (n > 0) println("SeedTables: swept up " + n + " routines nothing names");
        return n;
    }

    // ROUTINES NAMED ONLY AS A NUMBER. An object's handler is stored with  move.l #routine,(4,A1)  and called later as  jsr (A4);
    // others sit in tables of records in the data. Every long in the image (at each even address: instruction operands and data
    // alike) that is the address straight after a routine's end (rts / rte / an instruction that does not fall through) and reads
    // as a routine there is taken as one.
    private boolean afterEnd(long t) throws Exception {
        int w = mem.getShort(toAddr(t - 2)) & 0xFFFF;
        if (w == 0x4E75 || w == 0x4E73 || w == 0x4E77) return true;
        Instruction b = currentProgram.getListing().getInstructionContaining(toAddr(t - 1));
        return b != null && b.getFallThrough() == null && b.getMaxAddress().getOffset() == t - 1;
    }
    // TABLES NOTHING POINTS AT. A table whose address reaches the jsr in a register from far away (jsr ([A1],D1.w*4)) cannot be
    // followed from the instruction. But it looks like a table: three or more longs in a row, outside any instruction, each the
    // address of something that reads as code and lies within 0x4000 of the table itself.
    private int runs() throws Exception {
        int n = 0;
        for (long ea = 0x400; ea + 12 <= size; ea += 2) {
            if (currentProgram.getListing().getInstructionContaining(toAddr(ea)) != null) continue;
            int len = 0;
            for (; len < 512 && ea + 4L * len + 4 <= size; len++) {
                long at = ea + 4L * len;
                if (currentProgram.getListing().getInstructionContaining(toAddr(at)) != null) break;
                long t = mem.getInt(toAddr(at)) & 0xFFFFFFFFL;
                if (!plausible(t) || Math.abs(t - ea) > 0x4000) break;
                if (t >= ea && t < at + 4) break;                           // points into the table itself
                Instruction c = currentProgram.getListing().getInstructionContaining(toAddr(t));
                if (c != null ? !c.getAddress().equals(toAddr(t)) : !reads(toAddr(t))) break;
            }
            if (len < 3) continue;
            for (int k = 0; k < len; k++) if (seed(mem.getInt(toAddr(ea + 4L * k)) & 0xFFFFFFFFL)) n++;
            ea += 4L * len - 2;
        }
        if (n > 0) println("SeedTables: " + n + " routines from tables nothing points at");
        return n;
    }

    private int pointers() throws Exception {
        int n = 0;
        java.util.HashSet<Long> seen = new java.util.HashSet<>();
        for (long ea = 0x400; ea + 4 <= size; ea += 2) {
            long t = mem.getInt(toAddr(ea)) & 0xFFFFFFFFL;
            if (t < 0x400 || t >= size || (t & 1) != 0 || !seen.add(t)) continue;
            Address a = toAddr(t);
            if (currentProgram.getListing().getInstructionContaining(a) != null) continue;
            // straight after a routine's end, or a NEAR pointer held in data (a table of records: routine, parameter, routine ...)
            boolean near = Math.abs(t - ea) <= 0x4000;
            if (!plausible(t) || !(afterEnd(t) || near)) { seen.remove(t); continue; }
            if (seed(t)) n++;
        }
        if (n > 0) println("SeedTables: " + n + " routines named only as a number");
        return n;
    }

    private final java.util.ArrayList<Long> userSeeds = new java.util.ArrayList<>();   // the seeds file's addresses (run())

    @Override
    public void run() throws Exception {
        mem = currentProgram.getMemory();
        size = mem.getSize();
        pseudo = new ghidra.app.util.PseudoDisassembler(currentProgram);
        int total = 0;
        String[] args = getScriptArgs();
        if (args.length > 0 && new File(args[0]).exists()) {
            // first the DATA ranges ("LO-HI data": a little table Ghidra read as code, swallowing a real entry point after it, Air
            // Combat 22's 0xD05E-0xD067 before the rts at 0xD068): cleared, and kept out of every pass, as the switch tables are
            BufferedReader rd = new BufferedReader(new FileReader(args[0]));
            String dl;
            while ((dl = rd.readLine()) != null) {
                String w = dl.trim().split("\\s+")[0];
                if (w.isEmpty() || w.startsWith("#") || !w.contains("-")) continue;
                String[] p = w.split("-");
                long lo = Long.parseLong(p[0], 16), hi = Long.parseLong(p[1], 16);
                switchTables.put(lo, hi);
                clearListing(toAddr(lo), toAddr(hi));
            }
            rd.close();
            BufferedReader r = new BufferedReader(new FileReader(args[0]));
            String line; int n = 0;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#") || line.split("\\s+")[0].contains("-")) continue;
                long t = Long.parseLong(line.split("\\s+")[0], 16);
                if (plausible(t)) userSeeds.add(t);
                if (plausible(t) && seed(t, true)) n++;
            }
            r.close();
            println("SeedTables: " + n + " seeds from " + args[0]);
            total += n;
        }
        for (int round = 1; round <= 20; round++) {
            int added = 0, tables = 0;
            InstructionIterator ii = currentProgram.getListing().getInstructions(true);
            java.util.ArrayList<Long> targets = new java.util.ArrayList<>(), forced = new java.util.ArrayList<>();
            java.util.ArrayList<long[]> clears = new java.util.ArrayList<>();
            while (ii.hasNext()) {
                Instruction in = ii.next();
                if (in.getLength() < 4) continue;
                long a = in.getAddress().getOffset();
                int op = mem.getShort(toAddr(a)) & 0xFFFF;
                boolean viaReg = (op & 0xFFB8) == 0x4EB0;                    // jsr / jmp (d8,An,Xn): the table's address is in An
                if (op != 0x4EBB && op != 0x4EFB && !viaReg) continue;       // jsr / jmp (d8,PC,Xn) and its full-format forms
                int ext = mem.getShort(toAddr(a + 2)) & 0xFFFF;
                if ((op == 0x4EFB || op == 0x4EBB) && (ext & 0x100) == 0) {  // a SWITCH (Alpine Racer 2's compiler):  move.w (d8,PC,Dn.w*2),Dn
                                                                             // (jsr too: Ridge Racer's per-object state calls, 0xCF76 / 0x1DA3C)
                    boolean sw = false;                                      //  [nop]  jmp (d8,PC,Dn.w) -- a table of 16-bit offsets from its own start
                    Instruction b = in.getPrevious();
                    for (int k = 0; k < 3 && b != null; k++, b = b.getPrevious())
                        if (((mem.getShort(b.getAddress()) & 0xF1BF) == 0x303B)) { sw = true; break; }
                    if (!sw) continue;
                    long table = a + 2 + (byte) (ext & 0xFF);
                    if (table < 0 || table + 2 > size) continue;
                    tables++;
                    // the case count, from the bound test before it:  subq.l #n,Dn / cmpi.l #n,Dn / cmpi.w #n,Dn, then bhi -> n + 1 cases
                    int count = -1;
                    b = in.getPrevious();
                    for (int k = 0; k < 6 && b != null && count < 0; k++, b = b.getPrevious()) {
                        long ba = b.getAddress().getOffset();
                        int bop = mem.getShort(toAddr(ba)) & 0xFFFF;
                        if ((bop & 0xF1F0) == 0x5180) { int q = (bop >> 9) & 7; count = (q == 0 ? 8 : q) + 1; }
                        else if ((bop & 0xFFF8) == 0x0C80) count = mem.getInt(toAddr(ba + 2)) + 1;
                        else if ((bop & 0xFFF8) == 0x0C40) count = (mem.getShort(toAddr(ba + 2)) & 0xFFFF) + 1;
                        else if ((bop & 0xF1FF) == 0xB1FC || (bop & 0xF1FF) == 0xB0BC) count = mem.getInt(toAddr(ba + 2)) + 1;      // cmpa.l / cmp.l #n
                        else if ((bop & 0xF1FF) == 0xB07C) count = (mem.getShort(toAddr(ba + 2)) & 0xFFFF) + 1;                    // cmp.w #n
                    }
                    if (count > 0 && count <= 256) {                         // known size: the table is DATA -- clear any code Ghidra made of it
                        clears.add(new long[] { table, table + 2L * count - 1, a });   // (cleared after the scan: not under its iterator)
                        for (int k = 0; k < count; k++) {
                            long t = table + (short) mem.getShort(toAddr(table + 2L * k));
                            if (plausible(t)) forced.add(t);
                        }
                        continue;
                    }
                    // no bound test (a state machine's index): the table runs up to the first routine it names. Ghidra often made
                    // instructions of the table itself (0x0037 0x00xx words read as ori.b ...): those do not end it -- only a real
                    // routine does (a function, or an odd / implausible offset) -- and they are cleared so the cases disassemble
                    long stop = size; int k = 0;
                    for (; k < 256; k++) {
                        long ea = table + 2L * k;
                        if (ea + 2 > size || ea + 2 > stop) break;
                        if (k > 0 && currentProgram.getFunctionManager().getFunctionAt(toAddr(ea)) != null) break;
                        long t = table + (short) mem.getShort(toAddr(ea));
                        if (!plausible(t) || (t & 1) != 0) break;            // (a case may sit before the dispatch: Ridge Racer's 0xA776 -> 0xA4EE)
                        if (stop == size && k >= 64) break;                  // no case after the table yet: not a table this long
                        if (t > table && t < stop) stop = t;
                        targets.add(t);
                    }
                    if (k > 0) clears.add(new long[] { table, table + 2L * k - 1, a });
                    continue;
                }
                if ((ext & 0x100) == 0 || (ext & 0x40) != 0) continue;       // brief format, or no index
                int iis = ext & 7;
                if (iis < 1 || iis > 3) continue;                            // not memory indirect pre-indexed
                int bdsize = (ext >> 4) & 3;
                long bd = bdsize == 2 ? (short) mem.getShort(toAddr(a + 4)) : bdsize == 3 ? mem.getInt(toAddr(a + 4)) : 0;
                if ((ext & 0x80) != 0) continue;                             // base suppressed: not PC-relative
                long table = a + 2 + bd;
                if (viaReg) {                                                // look back for  lea (d16,PC),An  /  lea (abs).l,An
                    int reg = op & 7; table = -1;
                    Instruction b = in.getPrevious();
                    for (int k = 0; k < 8 && b != null; k++, b = b.getPrevious()) {
                        long ba = b.getAddress().getOffset();
                        int bop = mem.getShort(toAddr(ba)) & 0xFFFF;
                        if (bop == (0x41FA | reg << 9)) { table = ba + 2 + (short) mem.getShort(toAddr(ba + 2)) + bd; break; }
                        if (bop == (0x41F9 | reg << 9)) { table = (mem.getInt(toAddr(ba + 2)) & 0xFFFFFFFFL) + bd; break; }
                    }
                    if (table < 0) continue;
                }
                if (table < 0 || table + 4 > size) continue;
                tables++;
                long stop = size;                                            // a table ends where the nearest routine it names begins
                for (int k = 0; k < 512; k++) {
                    long ea = table + 4L * k;
                    if (ea + 4 > size || ea + 4 > stop) break;
                    if (k > 0 && currentProgram.getListing().getInstructionAt(toAddr(ea)) != null) break;   // the table ran into code
                    long t = mem.getInt(toAddr(ea)) & 0xFFFFFFFFL;
                    if (!plausible(t)) break;
                    if (t > table && t < stop) stop = t;
                    targets.add(t);
                }
            }
            for (long[] c : clears) {
                switchTables.put(c[0], c[1]);
                Address t0 = toAddr(c[0]), t1 = toAddr(c[1]);
                for (ghidra.program.model.symbol.Reference r : getReferencesFrom(toAddr(c[2])))   // Ghidra's guess at the jmp's target: the table
                    if (r.getToAddress().compareTo(t0) >= 0 && r.getToAddress().compareTo(t1) <= 0) removeReference(r);
                for (Instruction v = currentProgram.getListing().getInstructionContaining(t0); v != null && v.getMinAddress().compareTo(t1) <= 0;
                     v = currentProgram.getListing().getInstructionAfter(v.getMaxAddress())) {
                    ghidra.program.model.listing.Function f = currentProgram.getFunctionManager().getFunctionAt(v.getAddress());
                    if (f != null) removeFunction(f);
                    currentProgram.getListing().clearCodeUnits(v.getMinAddress(), v.getMaxAddress(), false);
                    carved++;
                }
            }
            for (long t : targets) if (seed(t)) added++;
            for (long t : forced) if (currentProgram.getListing().getInstructionAt(toAddr(t)) == null && seed(t, true)) added++;
            added += repair();
            for (int k = sweep(); k > 0; k = sweep()) added += k;
            added += pointers();
            added += runs();
            println("SeedTables: round " + round + ": " + tables + " tables, " + added + " new entry points");
            total += added;
            if (added == 0) break;
            analyzeAll(currentProgram);
        }
        // THE SEEDS AGAIN: a table pass can clear code a seed made (Ridge Racer's 0x7568, inside a range read as a table) -- in a project
        // seeded one round at a time it survived, from a fresh import it did not. A seed is an address the program was SEEN to run.
        for (int pass = 0; pass < 3; pass++) {
            int back = 0;
            for (long t : userSeeds)
                if (currentProgram.getListing().getInstructionAt(toAddr(t)) == null && seed(t, true)) back++;
            if (back == 0) break;
            println("SeedTables: " + back + " seeds put back after the table passes");
            total += back;
            analyzeAll(currentProgram);
        }
        for (long t : userSeeds)
            if (currentProgram.getListing().getInstructionAt(toAddr(t)) == null) println(String.format("SeedTables: seed %X is still not code", t));
        println("SeedTables: " + total + " entry points added in all, " + carved + " wrong instructions cleared");
    }
}
