use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::quote;
use syn::parse::{Parse, ParseStream};
use syn::punctuated::Punctuated;
use syn::{ExprPath, FnArg, GenericArgument, Ident, ItemFn, LitStr, Pat, PathArguments, Token, Type, TypeReference, parse_macro_input};

struct CommandAttr {
    desc: LitStr,
}

impl Parse for CommandAttr {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        Ok(Self { desc: input.parse()? })
    }
}

struct ArgAttr {
    desc: LitStr,
    values: Option<Vec<ExprPath>>,
}

impl Parse for ArgAttr {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let desc: LitStr = input.parse()?;
        let mut values = None;
        while input.peek(Token![,]) {
            input.parse::<Token![,]>()?;
            if input.is_empty() {
                break;
            }
            let key: Ident = input.parse()?;
            input.parse::<Token![=]>()?;
            if key != "values" {
                return Err(syn::Error::new(key.span(), "expected `values`"));
            }
            let content;
            syn::bracketed!(content in input);
            let paths = Punctuated::<ExprPath, Token![,]>::parse_terminated(&content)?;
            values = Some(paths.into_iter().collect());
        }
        Ok(Self { desc, values })
    }
}

enum ArgKind {
    Int,
    Str,
    CStr,
    OptionalInt,
    OptionalStr,
    OptionalCStr,
    Choice(Vec<(ExprPath, String)>),
}

struct ParsedArg {
    name: Ident,
    desc: LitStr,
    kind: ArgKind,
    ty: Type,
}

fn is_path_ident(ty: &Type, name: &str) -> bool {
    matches!(ty, Type::Path(p) if p.path.is_ident(name))
}

fn is_ref_to(ty: &Type, name: &str) -> bool {
    matches!(ty, Type::Reference(TypeReference { elem, .. }) if is_path_ident(elem, name))
}

fn option_inner(ty: &Type) -> Option<&Type> {
    let Type::Path(p) = ty else {
        return None;
    };
    let seg = p.path.segments.last()?;
    if seg.ident != "Option" {
        return None;
    }
    if let PathArguments::AngleBracketed(args) = &seg.arguments
        && let Some(GenericArgument::Type(inner)) = args.args.first()
    {
        return Some(inner);
    }
    None
}

fn to_screaming_snake(s: &str) -> String {
    let mut result = String::new();
    for (i, c) in s.chars().enumerate() {
        if c.is_uppercase() && i > 0 {
            result.push('_');
        }
        result.push(c.to_ascii_uppercase());
    }
    result
}

fn classify(ty: &Type, attr: &ArgAttr) -> ArgKind {
    if let Some(values) = &attr.values {
        let choices = values
            .iter()
            .map(|path| {
                let variant = &path.path.segments.last().expect("expected path like Type::Variant").ident;
                (path.clone(), to_screaming_snake(&variant.to_string()))
            })
            .collect();
        return ArgKind::Choice(choices);
    }
    if is_path_ident(ty, "isize") {
        return ArgKind::Int;
    }
    if is_ref_to(ty, "str") {
        return ArgKind::Str;
    }
    if is_ref_to(ty, "CStr") {
        return ArgKind::CStr;
    }
    if let Some(inner) = option_inner(ty) {
        if is_path_ident(inner, "isize") {
            return ArgKind::OptionalInt;
        }
        if is_ref_to(inner, "str") {
            return ArgKind::OptionalStr;
        }
        if is_ref_to(inner, "CStr") {
            return ArgKind::OptionalCStr;
        }
    }
    panic!("cannot infer console arg type for `{}`. Use #[arg(\"desc\", values = [...])] for enum args.", quote!(#ty));
}

fn parse_args(func: &ItemFn) -> Vec<ParsedArg> {
    func.sig
        .inputs
        .iter()
        .map(|input| {
            let FnArg::Typed(pat_type) = input else {
                panic!("self parameters not supported");
            };
            let Pat::Ident(pat_ident) = &*pat_type.pat else {
                panic!("pattern parameters not supported");
            };
            let attr: ArgAttr = pat_type
                .attrs
                .iter()
                .find(|a| a.path().is_ident("arg"))
                .expect("missing #[arg(\"description\")] on parameter")
                .parse_args()
                .expect("invalid #[arg(...)] syntax");
            let kind = classify(&pat_type.ty, &attr);
            ParsedArg { name: pat_ident.ident.clone(), desc: attr.desc, kind, ty: (*pat_type.ty).clone() }
        })
        .collect()
}

#[proc_macro_attribute]
pub fn console_command(attr: TokenStream, item: TokenStream) -> TokenStream {
    let cmd_attr = parse_macro_input!(attr as CommandAttr);
    let func = parse_macro_input!(item as ItemFn);
    let args = parse_args(&func);
    let cmd_name = &func.sig.ident;
    let name = LitStr::new(&cmd_name.to_string(), cmd_name.span());
    let desc = &cmd_attr.desc;
    let body = &func.block;

    let specs = args.iter().map(|arg| {
        let name = LitStr::new(&arg.name.to_string(), arg.name.span());
        let desc = &arg.desc;
        match &arg.kind {
            ArgKind::Int => quote!(::anchor_console::ArgSpec::int(#name, #desc)),
            ArgKind::Str | ArgKind::CStr => quote!(::anchor_console::ArgSpec::str(#name, #desc)),
            ArgKind::OptionalInt => quote!(::anchor_console::ArgSpec::int(#name, #desc).optional()),
            ArgKind::OptionalStr | ArgKind::OptionalCStr => quote!(::anchor_console::ArgSpec::str(#name, #desc).optional()),
            ArgKind::Choice(choices) => {
                let names = choices.iter().map(|(_, name)| name);
                quote!(::anchor_console::ArgSpec::choice(#name, #desc, &[#(#names),*]))
            }
        }
    });

    let extractions = args.iter().enumerate().map(|(i, arg)| {
        let name = &arg.name;
        match &arg.kind {
            ArgKind::Int => quote! { let #name = __args.int(#i); },
            ArgKind::Str => quote! { let #name = __args.str(#i); },
            ArgKind::CStr => quote! { let #name = __args.c_str(#i); },
            ArgKind::OptionalInt => quote! { let #name = __args.optional_int(#i); },
            ArgKind::OptionalStr => quote! { let #name = __args.optional_str(#i); },
            ArgKind::OptionalCStr => quote! { let #name = __args.optional_c_str(#i); },
            ArgKind::Choice(choices) => {
                let (last, rest) = choices.split_last().expect("values must not be empty");
                let arms = rest.iter().enumerate().map(|(index, (path, _))| quote! { #index => #path, });
                let last = &last.0;
                quote! { let #name = match __args.choice(#i) { #(#arms)* _ => #last }; }
            }
        }
    });

    let completions = args
        .iter()
        .find_map(|arg| match &arg.kind {
            ArgKind::Choice(choices) => {
                let names = choices.iter().map(|(_, name)| name);
                Some(quote!(&[#(#names),*]))
            }
            _ => None,
        })
        .unwrap_or_else(|| quote!(&[]));

    let params: Vec<TokenStream2> = args
        .iter()
        .map(|arg| {
            let name = &arg.name;
            let ty = &arg.ty;
            quote!(#name: #ty)
        })
        .collect();
    let names: Vec<&Ident> = args.iter().map(|arg| &arg.name).collect();
    let args_binding = if args.is_empty() { quote!(_) } else { quote!(__args) };

    quote! {
        #[allow(dead_code)]
        mod #cmd_name {
            #[allow(unused_imports)]
            use super::*;

            fn __handler(#(#params),*) #body

            fn __run(#args_binding: &::anchor_console::Args<'_>, _: &mut ::anchor_console::Output<'_>) {
                #(#extractions)*
                __handler(#(#names),*);
            }

            pub static COMMAND: ::anchor_console::Command = ::anchor_console::Command {
                name: #name,
                desc: #desc,
                args: &[#(#specs),*],
                completions: #completions,
                handler: __run,
            };
        }
    }
    .into()
}
