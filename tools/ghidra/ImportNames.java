// Names the functions and globals of a Skyrim build from a CSV of `rva,name`
// lines (tools/names.py --csv <version>): what CommonLibSSE-NG and our
// src/game/Addresses.h call each Address Library ID. `Class::Method` goes into
// the namespace `Class`; a name at an address with no function becomes its
// primary label. A name set by hand in Ghidra is left alone: a finding made
// here outranks one read from a header. Idempotent, so tools/ghidra-mcp.ps1
// runs it on every launch and a name added to either source appears there.
//@category FollowerTactics

import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolUtilities;

public class ImportNames extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length != 1) {
			printerr("usage: ImportNames.java <csv of rva,name>");
			return;
		}
		Address base = currentProgram.getImageBase();
		int functions = 0, labels = 0, kept = 0, failed = 0;
		for (String line : Files.readAllLines(Path.of(args[0]))) {
			int comma = line.indexOf(',');
			if (comma < 0) {
				continue;
			}
			Address at = base.add(Long.parseLong(line.substring(0, comma), 16));
			String full = line.substring(comma + 1).trim();
			int split = full.lastIndexOf("::");
			String simple = SymbolUtilities.replaceInvalidChars(
				split < 0 ? full : full.substring(split + 2), true);
			try {
				Namespace ns = split < 0 ? currentProgram.getGlobalNamespace()
						: NamespaceUtils.createNamespaceHierarchy(full.substring(0, split), null,
							currentProgram, SourceType.IMPORTED);
				Function function = getFunctionAt(at);
				Symbol symbol = function != null ? function.getSymbol()
						: currentProgram.getSymbolTable().getPrimarySymbol(at);
				if (symbol != null && symbol.getSource() == SourceType.USER_DEFINED) {
					kept++;
				}
				else if (function != null) {
					symbol.setNameAndNamespace(simple, ns, SourceType.IMPORTED);
					functions++;
				}
				else {
					currentProgram.getSymbolTable()
							.createLabel(at, simple, ns, SourceType.IMPORTED)
							.setPrimary();
					labels++;
				}
			}
			catch (Exception e) {
				printerr(full + " at " + at + ": " + e.getMessage());
				failed++;
			}
		}
		println(String.format("ImportNames: %d functions, %d labels, %d named by hand kept, %d failed",
			functions, labels, kept, failed));
	}
}
