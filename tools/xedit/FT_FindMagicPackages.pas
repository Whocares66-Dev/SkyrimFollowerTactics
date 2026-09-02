{
  FollowerTactics -- discovery pass, writes nothing to any plugin.

  Finds the vanilla AI packages that cast magic, and dumps the shape of one so
  we know what a CastMagic package actually contains before trying to build or
  copy one. Copying a record the game already ships is far safer than authoring
  a PACK from scratch: package templates store their inputs as named, UID-tagged
  slots, and getting that structure subtly wrong yields a plugin that loads and
  then quietly does nothing.

  Output goes to a file rather than the message window, because -autoexit closes
  the window before anyone can read it.
}
unit UserScript;

const
  REPORT = 'C:\project\SkyrimFollowerTactics\SSEEdit\ft_packages.txt';

var
  sl: TStringList;

// Every top-level field of a record, one per line, so we can see the real
// element names instead of guessing them.
procedure DumpElements(r: IInterface; indent: string; depth: integer);
var
  i: integer;
  e: IInterface;
  v: string;
begin
  if depth > 3 then Exit;
  for i := 0 to Pred(ElementCount(r)) do begin
    e := ElementByIndex(r, i);
    v := GetEditValue(e);
    sl.Add(indent + Name(e) + ' = ' + v);
    if ElementCount(e) > 0 then
      DumpElements(e, indent + '    ', depth + 1);
  end;
end;

function Initialize: integer;
var
  i, j, found: integer;
  f, g, r: IInterface;
  t, edid: string;
begin
  sl := TStringList.Create;
  sl.Add('FollowerTactics package discovery');
  sl.Add('');

  found := 0;
  for i := 0 to Pred(FileCount) do begin
    f := FileByIndex(i);
    if GetFileName(f) <> 'Skyrim.esm' then Continue;

    g := GroupBySignature(f, 'PACK');
    sl.Add('Skyrim.esm PACK records: ' + IntToStr(ElementCount(g)));
    sl.Add('');

    for j := 0 to Pred(ElementCount(g)) do begin
      r := ElementByIndex(g, j);
      t := GetElementEditValues(r, 'PKDT\Type');
      edid := EditorID(r);

      if (Pos('Magic', t) > 0) or (Pos('Magic', edid) > 0) or (Pos('Cast', edid) > 0) then begin
        Inc(found);
        sl.Add('[' + IntToStr(found) + '] ' + edid + '   type=' + t
               + '   template=' + GetElementEditValues(r, 'PNAM'));

        // Dump the first few in full -- that is where the input names live.
        if found <= 3 then begin
          sl.Add('    ---- full structure ----');
          DumpElements(r, '    ', 0);
          sl.Add('    ------------------------');
          sl.Add('');
        end;
      end;
    end;
  end;

  sl.Add('');
  sl.Add('matches: ' + IntToStr(found));
  sl.SaveToFile(REPORT);
  sl.Free;
  Result := 1;   // stop here; nothing to iterate
end;

end.
