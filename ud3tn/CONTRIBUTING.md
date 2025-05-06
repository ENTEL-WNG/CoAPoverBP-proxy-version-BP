# Introduction

Thank you for your interest in contributing to µD3TN!

If you have any questions or need support in deploying µD3TN, please reach out
to the maintainers directly via contact@d3tn.com before opening an issue.
D3TN GmbH also offers commercial support and adaptation services for µD3TN.

The processes documented here might change over time, thus, please regularly
check this document for updates.

# Submission of Issues and Enhancement Proposals

Please don't hesitate to open new issues on our issue tracker, provided that
the subject of submission is not already covered in any existing one.
In the issue body, please indicate the problem along with your expectations
(what should happen instead) and any conditions that might have an influence
as detailed as possible.

If you found a security-related bug, please mark the issue as confidential or
contact the maintainers directly.

# Submission of Changes

- Before you create a merge request, please make sure that the subject of change
  is covered by an issue.
- Please take note of the conventions noted below and make sure to apply them
  if possible. Deviations are fine as long as they are reasonably explained in
  the merge request and the reviewers agree to that reasoning.
- Only submissions conformant to the *Developer Certificate of Origin* can be
  accepted into µD3TN. Please see the DCO Version 1.1 for details, available at
  <DCO.txt> or via https://developercertificate.org/.
  When making a contribution you have to explicitly agree to these terms,
  using your real name. For this purpose, append a sign-off at the end of all
  Git commits in a format like `Signed-off-by: Joe Smith <joe.smith@email.com>`.
  This can be done automatically, if you set `user.name` and `user.email` in
  your Git configuration and pass the `-s` option to `git commit`.
- With your submission you grant D3TN GmbH the rights to use the submitted code
  under the terms of the Apache 2.0 license. Please also take note of the
  contained patent grant, if applicable.
  To be clear, D3TN GmbH may reuse, modify, and publish your code under any
  open-source or proprietary license that is compatible. This includes the
  currently-used AGPLv3 license of the µD3TN project and D3TN's proprietary
  licensing scheme for closed-source products.
- Please use the provided [merge request template](.gitlab/merge_request_templates/MR.md).

## Coding Guildelines

* Declare and document public interfaces cleanly
* Use constants whenever possible
* Avoid global state
* Provide appropriate tests along with new functionality
* Avoid complicated, complex, license-wise incompatible, or unmaintained
  dependencies
* Care for POSIX compatibility (e.g., Linux-only features can be disabled during
  build)
* Respect the [Linux Kernel Coding Style](https://www.kernel.org/doc/html/v5.7/process/coding-style.html)
  for C and [PEP8](https://www.python.org/dev/peps/pep-0008/) for Python

## Git(Lab) Workflow Guidelines

### Issues

* Titles of issues should preferably describe the _issue_
* Explain what should be done and why
* Reference any other issue and merge request(s) at the bottom

### Branches

* Format branch names like this: `<type>/<issue ID>-short-description`, whereas
  - `<type>` is one of `hotfix` and `feature`, and
  - `issue ID` should reference something from the
    [issue tracker](https://gitlab.com/d3tn/ud3tn/-/issues) -- if there is no
    issue, create one or drop that part including the dash
* Keep feature branches atomic if possible (one branch and merge request per
  feature / issue)

### Merge Requests

* Think about creating issues before submitting merge requests; if there is no issue, document the reason for the change
* Titles of merge requests should be in imperative mood like commit headings (see below)
* Keep merge requests in [`Draft` state](https://docs.gitlab.com/ee/user/project/merge_requests/drafts.html) (`WIP` is deprecated) as long as something is blocking them
* Explain what should be/has been done and why
* Reference any other issue and merge request(s) at the bottom
* Always assign merge requests to somebody (the one who should work/act on it next)

### Commits

* [Write good commit messages](https://chris.beams.io/posts/git-commit)
* Title: Use a short, descriptive title and [sentence case](https://en.wikipedia.org/wiki/Letter_case#Sentence_case)
  without a dot at the end
* Explain what has been done and why
* Merge commits should contain a reference to any existing merge request in the
  description
* Merge commits that update a *feature* branch with new commits from `master`
  should mention why they are necessary (why `git rebase` was not used)
* Refer to the issue tracker if possible
* Add a sign-off line to your commits to declare conformity with these rules,
  do NOT sign the commits with GPG to facilitate easy rebases by others

An ideal commit should look similar to the following:

```
<TITLE LINE>

<WHAT HAS BEEN CHANGED>

<WHY HAS THIS BEEN CHANGED / WHAT PROBLEM IS SOLVED WITH THE CHANGE>

<OTHER REMARKS / IMPLEMENTATION TRADE-OFFS>

Closes: <REFERENCE ISSUES THAT ARE RESOLVED WITH THIS OR RELATED>
<SIGN-OFF>
```
