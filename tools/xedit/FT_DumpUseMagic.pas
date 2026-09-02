{
  FollowerTactics -- second discovery pass, still writes nothing.

  The first pass suggested a UseMagic package carries no spell reference at all.
  That is a load-bearing claim -- it decides whether we need one package record
  per spell, one per follower, or exactly one -- so it gets checked properly
  rather than inferred from a dump that was depth-limited.

  Three questions:
    1. What does the UseMagic template record itself contain, including the
       NAMES of its inputs (the name map), not just their types?
    2. What does a concrete package built from it fill in?
    3. Do any of them reference a SPEL form anywhere?
}
unit UserScript;

const
  REPORT = 'C:\project\SkyrimFollowerTactics\SSEEdit\ft_usemagic.txt';

var
  sl: TStringList;
  spellRefs, scanned: integer;

procedure Dump(r: IInterface; indent: string; depth: integer);
var
  i: integer;
  e: IInterface;
  v: string;
begin
  if depth > 8 then Exit;
  for i := 0 to Pred(ElementCount(r)) do begin
    e := ElementByIndex(r, i);
    v := GetEditValue(e);
    if (v = '') and (ElementCount(e) = 0) then
      sl.Add(indent + Name(e))
    else
      sl.Add(indent + Name(e) + ' = ' + v);
    if ElementCount(e) > 0 then Dump(e, indent + '  ', depth + 1);
  end;
end;

// Does this record mention a spell anywhere in its data inputs? Walks the tree
// looking for a value naming a SPEL form, rather than trusting one field path.
function MentionsSpell(r: IInterface; depth: integer): boolean;
var
  i: integer;
  e: IInterface;
  v: string;
begin
  Result := false;
  if depth > 8 then Exit;
  for i := 0 to Pred(ElementCount(r)) do begin
    e := ElementByIndex(r, i);
    v := GetEditValue(e);
    if (Pos('SPEL:', v) > 0) or (Pos('[SPEL', v) > 0) then begin
      Result := true;
      Exit;
    end;
    if ElementCount(e) > 0 then
      if MentionsSpell(e, depth + 1) then begin
        Result := true;
        Exit;
      end;
  end;
end;

function Initialize: integer;
var
  i, j, dumped: integer;
  f, g, r: IInterface;
  edid: string;
begin
  sl := TStringList.Create;
  spellRefs := 0;
  scanned := 0;
  dumped := 0;

  for i := 0 to Pred(FileCount) do begin
    f := FileByIndex(i);
    if GetFileName(f) <> 'Skyrim.esm' then Continue;
    g := GroupBySignature(f, 'PACK');

    for j := 0 to Pred(ElementCount(g)) do begin
      r := ElementByIndex(g, j);
      edid := EditorID(r);

      // (1) the templates themselves -- this is where input NAMES live
      if (edid = 'UseMagic') or (edid = 'UseMagicRepeat') then begin
        sl.Add('================ TEMPLATE: ' + edid + '  ' + IntToStr(FormID(r)) + ' ================');
        Dump(r, '', 0);
        sl.Add('');
        Continue;
      end;

      if Pos('UseMagic', GetElementEditValues(r, 'PKCU\Package Template')) = 0 then Continue;

      Inc(scanned);
      if MentionsSpell(r, 0) then Inc(spellRefs);

      // (2) two concrete examples, in full
      if dumped < 2 then begin
        Inc(dumped);
        sl.Add('================ CONCRETE: ' + edid + ' ================');
        Dump(r, '', 0);
        sl.Add('');
      end;
    end;
  end;

  sl.Add('');
  sl.Add('UseMagic packages scanned : ' + IntToStr(scanned));
  sl.Add('...that reference a SPEL  : ' + IntToStr(spellRefs));
  sl.SaveToFile(REPORT);
  sl.Free;
  Result := 1;
end;

end.
