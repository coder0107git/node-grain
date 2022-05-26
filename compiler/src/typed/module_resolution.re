open Grain_parsing;
open Grain_utils;
open Cmi_format;

type error =
  | Missing_module(Location.t, Path.t, Path.t)
  | No_module_file(string, option(string));

exception Error(error);

let error = err => raise(Error(err));

type module_location_result =
  | GrainModule(Filepath.t, option(Filepath.t)) /* Grain Source file, Compiled object */
  | WasmModule(Filepath.t); /* Compiled object */

let compile_module_dependency: ref((Filepath.t, Filepath.t) => unit) =
  ref((filename, output_file) =>
    failwith("compile_module Should be filled in by compile.re")
  );

let with_preserve_unit:
  ref((~loc: Location.t, string, Filepath.t, unit => unit) => unit) =
  ref((~loc, _, _, _) =>
    failwith("with_preserve_unit should be filled in by env.re")
  );

let current_unit_name: ref(unit => string) =
  ref(() => failwith("current_unit_name should be filled in by env.re"));

let current_filename: ref(unit => Filepath.t) =
  ref(() => failwith("current_filename should be filled in by env.re"));

let last_modified = Fs_access.last_modified;

let file_older = (a, b) => {
  last_modified(a) < last_modified(b);
};

let cmi_cache: Hashtbl.t(Filepath.t, cmi_infos) = Hashtbl.create(16);
let read_file_cmi = f => {
  switch (Hashtbl.find_opt(cmi_cache, f)) {
  | Some(cmi) => cmi
  | None =>
    let cmi = Cmi_format.read_cmi(Filepath.to_string(f));
    Hashtbl.add(cmi_cache, f, cmi);
    cmi;
  };
};

let get_output_name = name => {
  let dir = Filepath.dirname(name);
  let (basename, ext) = Filepath.basename(name);
  Filepath.append(dir, basename ++ ".gr.wasm");
};

let find_ext_in_dir = (dir, name) => {
  let rec process_ext =
    fun
    | [] => None
    | [ext, ..._]
        when Fs_access.file_exists(Filepath.append(dir, name ++ ext)) =>
      Some((Filepath.append(dir, name ++ ext), dir, name, ext))
    | [_, ...tl] => process_ext(tl);
  process_ext;
};

let find_in_path_uncap = (~exts=[], base_dir, path, name) => {
  let rec try_dir =
    fun
    | [] => raise(Not_found)
    | [dir, ...rem] => {
        switch (find_ext_in_dir(dir, name, exts)) {
        | Some(path) => path
        | None => try_dir(rem)
        };
      };
  if (Filepath.is_absolute(name)
      && Fs_access.file_exists(Filepath.from_absolute_string(name))) {
    let abs_path = Filepath.from_absolute_string(name);
    let (basename, ext) = Filepath.basename(abs_path);
    (abs_path, Filepath.dirname(abs_path), basename, ext);
  } else if (Filepath.is_module(name)) {
    try(try_dir(path)) {
    | Not_found => raise(Not_found)
    };
  } else {
    try_dir([base_dir]);
  };
};

module PathTbl = {
  type t('a) = Hashtbl.t(Filepath.t, 'a);
  let create: int => t('a) = Hashtbl.create;

  let add: (t('a), (Filepath.t, string), 'a) => unit =
    (tbl, (dir, unit_name), v) => {
      Hashtbl.add(tbl, Filepath.append(dir, unit_name), v);
    };

  let find_opt:
    (
      ~disable_relpath: bool=?,
      t('a),
      Filepath.t,
      list(Filepath.t),
      string
    ) =>
    option('a) =
    (~disable_relpath=false, tbl, base_path, path, unit_name) =>
      if (!disable_relpath
          && Filepath.is_relative(unit_name)
          && !Filepath.is_module(unit_name)) {
        Hashtbl.find_opt(tbl, Filepath.append(base_path, unit_name));
      } else {
        List.fold_left(
          (acc, elt) => {
            switch (acc) {
            | Some(_) => acc
            | None => Hashtbl.find_opt(tbl, Filepath.append(elt, unit_name))
            }
          },
          None,
          path,
        );
      };
};

let located_module_cache:
  Hashtbl.t(Filepath.t, PathTbl.t(module_location_result)) =
  Hashtbl.create(16);
let resolutions: Hashtbl.t(Filepath.t, PathTbl.t(Filepath.t)) =
  Hashtbl.create(16);

let current_located_module_cache = () => {
  switch (Hashtbl.find_opt(located_module_cache, current_filename^())) {
  | Some(v) => v
  | None =>
    let new_table = PathTbl.create(12);
    Hashtbl.add(located_module_cache, current_filename^(), new_table);
    new_table;
  };
};
let current_resolution_table = () => {
  switch (Hashtbl.find_opt(resolutions, current_filename^())) {
  | Some(v) => v
  | None =>
    let new_table = PathTbl.create(12);
    Hashtbl.add(resolutions, current_filename^(), new_table);
    new_table;
  };
};

let log_resolution = (unit_name, dir, fullpath) => {
  PathTbl.add(current_resolution_table(), (dir, unit_name), fullpath);
  fullpath;
};

let resolve_unit = (~cache=true, ~base_dir=?, unit_name) => {
  let base_dir =
    switch (base_dir) {
    | None => Filepath.dirname(current_filename^())
    | Some(bd) => bd
    };
  let path = Grain_utils.Config.module_search_path();
  switch (
    cache,
    PathTbl.find_opt(current_resolution_table(), base_dir, path, unit_name),
  ) {
  | (true, Some(res)) => res
  | _ =>
    let exts = [".gr", ".gr.wasm"];
    let (fullpath, dir, basename, _) =
      find_in_path_uncap(~exts, base_dir, path, unit_name);
    if (cache) {
      log_resolution(unit_name, dir, fullpath);
    } else {
      fullpath;
    };
  };
};

let locate_module = (~disable_relpath=false, base_dir, path, unit_name) => {
  switch (
    PathTbl.find_opt(
      ~disable_relpath,
      current_located_module_cache(),
      base_dir,
      path,
      unit_name,
    )
  ) {
  | Some(m) => m
  | None =>
    let grain_src_exts = [".gr"];
    let (dir, m) =
      switch (
        find_in_path_uncap(~exts=[".gr.wasm"], base_dir, path, unit_name)
      ) {
      | (objpath, dir, basename, ext) =>
        ignore(log_resolution(unit_name, dir, objpath));
        switch (find_ext_in_dir(dir, basename, grain_src_exts)) {
        | Some((srcpath, _, _, _)) => (
            dir,
            GrainModule(srcpath, Some(objpath)),
          )
        | None => (dir, WasmModule(objpath))
        };
      | exception Not_found =>
        let (srcpath, dir, _, _) =
          find_in_path_uncap(~exts=grain_src_exts, base_dir, path, unit_name);
        (dir, GrainModule(srcpath, None));
      };
    PathTbl.add(current_located_module_cache(), (dir, unit_name), m);
    m;
  };
};

type dependency_node = {
  // dn_unit_name is a hashtable because we may have a situation
  // where A depends on B and C, and both B and C depend on D.
  // D will then have two unit names, corresponding to the "view" from B and C.
  dn_unit_name: Hashtbl.t(option(dependency_node), string), // <- node_where_imported: name_of_unit_where_imported
  dn_file_name: string,
  dn_up_to_date: ref(bool), // cached up_to_date check
  dn_latest_resolution: ref(option(module_location_result)),
};

let located_to_out_file_name = (~base=?, located) => {
  switch (located) {
  | GrainModule(srcpath, None) => get_output_name(srcpath)
  | GrainModule(_, Some(outpath))
  | WasmModule(outpath) => outpath
  };
};

let locate_unit_object_file = (~path=?, ~base_dir=?, unit_name) => {
  let base_dir =
    switch (base_dir) {
    | None => Filepath.dirname(current_filename^())
    | Some(bd) => bd
    };
  let path =
    switch (path) {
    | Some(p) => p
    | None => Grain_utils.Config.module_search_path()
    };
  located_to_out_file_name(locate_module(base_dir, path, unit_name));
};

module Dependency_graph =
  Dependency_graph.Make({
    type t = dependency_node;
    let compare = (dn1, dn2) =>
      String.compare(dn1.dn_file_name, dn2.dn_file_name);
    let hash = dn => Hashtbl.hash(dn.dn_file_name);
    let equal = (dn1, dn2) =>
      String.equal(dn1.dn_file_name, dn2.dn_file_name);

    let get_filename = dn => Filepath.from_absolute_string(dn.dn_file_name);

    let rec get_dependencies: (t, Filepath.t => option(t)) => list(t) =
      (dn, lookup) => {
        let base_dir =
          Filepath.dirname(Filepath.from_absolute_string(dn.dn_file_name));
        let active_search_path = Config.module_search_path();
        let located = dn.dn_latest_resolution^;

        let from_srcpath = srcpath => {
          List.map(
            name => {
              let located = locate_module(base_dir, active_search_path, name);
              let out_file_name = located_to_out_file_name(located);
              let existing_dependency = lookup(out_file_name);
              switch (existing_dependency) {
              | Some(ed) =>
                Hashtbl.add(ed.dn_unit_name, Some(dn), name);
                ed;
              | None =>
                let tbl = Hashtbl.create(8);
                Hashtbl.add(tbl, Some(dn), name);
                {
                  dn_unit_name: tbl,
                  dn_file_name: Filepath.to_string(out_file_name),
                  dn_up_to_date: ref(false), // <- needs to be checked
                  dn_latest_resolution: ref(Some(located)),
                };
              };
            },
            Grain_parsing.Driver.scan_for_imports(srcpath),
          );
        };

        // TODO: (#597) Propagating the compiler flag information correctly is tricky.
        //        For the moment, from the dependency graph's perspective, we assume that
        //        nothing uses --no-pervasives or --no-gc.
        switch (located) {
        | None => failwith("get_dependencies: Should be impossible")
        | Some(WasmModule(_)) => []
        | Some(GrainModule(srcpath, None)) => from_srcpath(srcpath)
        | Some(GrainModule(srcpath, Some(objpath))) =>
          switch (read_file_cmi(objpath)) {
          | exception (Cmi_format.Error(_)) => from_srcpath(srcpath)
          | cmi =>
            List.map(
              ((name, _)) => {
                let located =
                  locate_module(base_dir, active_search_path, name);
                let out_file_name = located_to_out_file_name(located);
                let existing_dependency = lookup(out_file_name);
                switch (existing_dependency) {
                | Some(ed) =>
                  Hashtbl.add(ed.dn_unit_name, Some(dn), name);
                  ed;
                | None =>
                  let tbl = Hashtbl.create(8);
                  Hashtbl.add(tbl, Some(dn), name);
                  {
                    dn_unit_name: tbl,
                    dn_file_name: Filepath.to_string(out_file_name),
                    dn_up_to_date: ref(false), // <- needs to be checked
                    dn_latest_resolution: ref(Some(located)),
                  };
                };
              },
              cmi.cmi_crcs,
            )
          }
        };
      };

    let check_up_to_date: t => unit =
      dn => {
        switch (dn.dn_up_to_date^, dn.dn_latest_resolution^) {
        | (true, _) => ()
        | (false, None)
        | (false, Some(GrainModule(_, None))) =>
          // File isn't compiled, so it's not up-to-date yet.
          dn.dn_up_to_date := false
        | (false, Some(WasmModule(_))) =>
          // WASM modules are always up-to-date
          dn.dn_up_to_date := true
        | (false, Some(GrainModule(srcpath, Some(objpath)))) =>
          // Compiled file is up-to-date if the srcpath is older than the objpath,
          // all dependencies have expected CRC, and the module was compiled with
          // the current compiler configuration. Otherwise, we need to recompile.
          let config_sum = Cmi_format.config_sum();
          let base_dir = Filepath.dirname(srcpath);
          dn.dn_up_to_date :=
            (
              switch (read_file_cmi(objpath)) {
              // Treat corrupted CMI as invalid
              | exception (Cmi_format.Error(_)) => false
              | cmi =>
                config_sum == cmi.cmi_config_sum
                && file_older(srcpath, objpath)
                && List.for_all(
                     ((name, crc)) => {
                       let resolved = resolve_unit(~base_dir, name);
                       let out_file_name = get_output_name(resolved);
                       Fs_access.file_exists(out_file_name)
                       && (
                         switch (crc) {
                         | None => false
                         | Some(crc) =>
                           try(
                             Cmi_format.cmi_to_crc(
                               read_file_cmi(out_file_name),
                             )
                             == crc
                           ) {
                           | _ => false
                           }
                         }
                       );
                     },
                     cmi.cmi_crcs,
                   )
              }
            );
        };
      };

    let is_up_to_date = dn => {
      dn.dn_up_to_date^;
    };

    let compile_module = (~loc=?, dn) => {
      let srcpath =
        switch (dn.dn_latest_resolution^) {
        | None => failwith("impossible: compile_module > None")
        | Some(WasmModule(_)) =>
          failwith("impossible: compile_module > WasmModule")
        | Some(GrainModule(srcpath, _)) => srcpath
        };
      let outpath = get_output_name(srcpath);
      let loc = Option.value(loc, ~default=Grain_parsing.Location.dummy_loc);
      let chosen_unit_name =
        switch (Hashtbl.to_seq(dn.dn_unit_name, ())) {
        | Seq.Nil => failwith("Impossible: empty dn_unit_name")
        | Seq.Cons((parent, unit_name), _) => unit_name
        };
      with_preserve_unit^(~loc, chosen_unit_name, srcpath, () =>
        Grain_utils.Warnings.with_preserve_warnings(() =>
          Grain_utils.Config.preserve_config(() =>
            compile_module_dependency^(srcpath, outpath)
          )
        )
      );
      dn.dn_latest_resolution := Some(GrainModule(srcpath, Some(outpath)));
      dn.dn_up_to_date := true;
      PathTbl.add(
        current_located_module_cache(),
        (Filepath.dirname(outpath), chosen_unit_name),
        GrainModule(srcpath, Some(outpath)),
      );
    };
  });

let locate_module_file = (~loc, ~disable_relpath=false, unit_name) => {
  let base_dir = Filepath.dirname(current_filename^());
  let path = Grain_utils.Config.module_search_path();
  let located =
    try(locate_module(~disable_relpath, base_dir, path, unit_name)) {
    | Not_found => error(No_module_file(unit_name, None))
    };
  let out_file = located_to_out_file_name(located);
  let current_dep_node =
    Dependency_graph.lookup_filename(current_filename^());
  let existing_dependency = Dependency_graph.lookup_filename(out_file);
  let dn =
    switch (existing_dependency) {
    | Some(ed) =>
      Hashtbl.add(ed.dn_unit_name, current_dep_node, unit_name);
      ed;
    | None =>
      let tbl = Hashtbl.create(8);
      Hashtbl.add(tbl, current_dep_node, unit_name);
      {
        dn_unit_name: tbl,
        dn_file_name: Filepath.to_string(out_file),
        dn_up_to_date: ref(false), // <- needs to be checked
        dn_latest_resolution: ref(Some(located)),
      };
    };
  Dependency_graph.register(dn);
  Dependency_graph.compile_dependencies(~loc, out_file);
  let ret = located_to_out_file_name(located);
  ret;
};

let clear_dependency_graph = () => {
  Dependency_graph.clear();
};

let () = {
  Fs_access.register_cache_flusher((
    Hashtbl.remove(cmi_cache),
    () => Hashtbl.clear(cmi_cache),
  ));
  Fs_access.register_cache_flusher((
    Hashtbl.remove(located_module_cache),
    () => Hashtbl.clear(located_module_cache),
  ));
  Fs_access.register_cache_flusher((
    Hashtbl.remove(resolutions),
    () => Hashtbl.clear(resolutions),
  ));
};

let dump_dependency_graph = Dependency_graph.dump;

/* Error report */

open Format;

let report_error = ppf =>
  fun
  | Missing_module(_, path1, path2) => {
      fprintf(ppf, "@[@[<hov>");
      if (Path.same(path1, path2)) {
        fprintf(ppf, "Internal path@ %s@ is dangling.", Path.name(path1));
      } else {
        fprintf(
          ppf,
          "Internal path@ %s@ expands to@ %s@ which is dangling.",
          Path.name(path1),
          Path.name(path2),
        );
      };
      fprintf(
        ppf,
        "@]@ @[%s@ %s@ %s.@]@]",
        "The compiled interface for module",
        Ident.name(Path.head(path2)),
        "was not found",
      );
    }
  | No_module_file(m, None) => fprintf(ppf, "Missing file for module %s", m)
  | No_module_file(m, Some(msg)) =>
    fprintf(ppf, "Missing file for module %s: %s", m, msg);

let () =
  Location.register_error_of_exn(
    fun
    | Error(Missing_module(loc, _, _) as err) when loc != Location.dummy_loc =>
      Some(Location.error_of_printer(loc, report_error, err))
    | Error(err) => Some(Location.error_of_printer_file(report_error, err))
    | _ => None,
  );

let () = Printexc.record_backtrace(true);
