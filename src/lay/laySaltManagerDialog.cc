
/*

  KLayout Layout Viewer
  Copyright (C) 2006-2017 Matthias Koefferlein

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#include "laySaltManagerDialog.h"
#include "laySaltModel.h"
#include "laySaltGrainPropertiesDialog.h"
#include "laySalt.h"
#include "ui_SaltGrainTemplateSelectionDialog.h"
#include "tlString.h"
#include "tlExceptions.h"
#include "tlHttpStream.h"

#include <QTextDocument>
#include <QPainter>
#include <QDir>
#include <QTextStream>
#include <QBuffer>
#include <QResource>
#include <QMessageBox>
#include <QAbstractItemModel>
#include <QStyledItemDelegate>

namespace lay
{

// --------------------------------------------------------------------------------------

/**
 *  @brief A tiny dialog to select a template and a name for the grain
 */
class SaltGrainTemplateSelectionDialog
  : public QDialog, private Ui::SaltGrainTemplateSelectionDialog
{
public:
  SaltGrainTemplateSelectionDialog (QWidget *parent, lay::Salt *salt)
    : QDialog (parent), mp_salt (salt)
  {
    Ui::SaltGrainTemplateSelectionDialog::setupUi (this);

    m_salt_templates.add_location (":/salt_templates");
    salt_view->setModel (new SaltModel (this, &m_salt_templates));
    salt_view->setItemDelegate (new SaltItemDelegate (this));
    salt_view->setCurrentIndex (salt_view->model ()->index (0, 0, QModelIndex ()));
  }

  lay::SaltGrain templ () const
  {
    SaltModel *model = dynamic_cast<SaltModel *> (salt_view->model ());
    tl_assert (model != 0);

    SaltGrain *g = model->grain_from_index (salt_view->currentIndex ());
    tl_assert (g != 0);

    return *g;
  }

  std::string name () const
  {
    return tl::to_string (name_edit->text ());
  }

  void accept ()
  {
    name_alert->clear ();
    std::string name = tl::to_string (name_edit->text ().simplified ());
    if (name.empty ()) {
      name_alert->error () << tr ("Name must not be empty");
    } else if (! SaltGrain::valid_name (name)) {
      name_alert->error () << tr ("Name is not valid (must be composed of letters, digits or underscores.\nGroups and names need to be separated with slashes.");
    } else {

      //  check, if this name does not exist yet
      for (Salt::flat_iterator g = mp_salt->begin_flat (); g != mp_salt->end_flat (); ++g) {
        if ((*g)->name () == name) {
          name_alert->error () << tr ("A package with this name already exists");
          return;
        }
      }

      QDialog::accept ();

    }
  }

private:
  lay::Salt m_salt_templates;
  lay::Salt *mp_salt;
};

// --------------------------------------------------------------------------------------
//  SaltManager implementation

// @@@
lay::Salt salt;
static bool salt_initialized = false;
void make_salt ()
{
  if (!salt_initialized) {
    salt_initialized = true;
    salt.add_location (tl::to_string (QDir::homePath () + QString::fromUtf8("/.klayout/salt")));
  }
}
lay::Salt *get_salt ()
{
  salt = lay::Salt (); salt_initialized = false;
  make_salt ();
  return &salt;
}
// @@@

// @@@
lay::Salt salt_mine;
void make_salt_mine ()
{
  salt_mine = lay::Salt ();
  salt_mine.load ("/home/matthias/salt.mine");
}
lay::Salt *get_salt_mine ()
{
  make_salt_mine();
  return &salt_mine;
}
// @@@

SaltManagerDialog::SaltManagerDialog (QWidget *parent)
  : QDialog (parent),
    m_current_changed_enabled (true)
{
  Ui::SaltManagerDialog::setupUi (this);
  mp_properties_dialog = new lay::SaltGrainPropertiesDialog (this);

  connect (edit_button, SIGNAL (clicked ()), this, SLOT (edit_properties ()));
  connect (create_button, SIGNAL (clicked ()), this, SLOT (create_grain ()));
  connect (delete_button, SIGNAL (clicked ()), this, SLOT (delete_grain ()));

  mp_salt = get_salt ();
  mp_salt_mine = get_salt_mine ();

  SaltModel *model;

  model = new SaltModel (this, mp_salt);
  salt_view->setModel (model);
  salt_view->setItemDelegate (new SaltItemDelegate (this));

  model = new SaltModel (this, mp_salt_mine);
  salt_mine_view->setModel (model);
  salt_mine_view->setItemDelegate (new SaltItemDelegate (this));

  mode_tab->setCurrentIndex (mp_salt->is_empty () ? 1 : 0);

  connect (mode_tab, SIGNAL (currentChanged (int)), this, SLOT (mode_changed ()));
  connect (mp_salt, SIGNAL (collections_changed ()), this, SLOT (salt_changed ()));
  connect (mp_salt_mine, SIGNAL (collections_changed ()), this, SLOT (salt_mine_changed ()));

  salt_changed ();
  salt_mine_changed ();

  connect (salt_view->selectionModel (), SIGNAL (currentChanged (const QModelIndex &, const QModelIndex &)), this, SLOT (current_changed ()));
  connect (salt_mine_view->selectionModel (), SIGNAL (currentChanged (const QModelIndex &, const QModelIndex &)), this, SLOT (mine_current_changed ()));

  search_installed_edit->set_clear_button_enabled (true);
  search_new_edit->set_clear_button_enabled (true);
  connect (search_installed_edit, SIGNAL (textChanged (const QString &)), this, SLOT (search_text_changed (const QString &)));
  connect (search_new_edit, SIGNAL (textChanged (const QString &)), this, SLOT (search_text_changed (const QString &)));
}

void
SaltManagerDialog::mode_changed ()
{
  //  keeps the splitters in sync
  if (mode_tab->currentIndex () == 1) {
    splitter_new->setSizes (splitter->sizes ());
  } else if (mode_tab->currentIndex () == 0) {
    splitter->setSizes (splitter_new->sizes ());
  }
}

void
SaltManagerDialog::search_text_changed (const QString &text)
{
  QListView *view = 0;
  if (sender () == search_installed_edit) {
    view = salt_view;
  } else if (sender () == search_new_edit) {
    view = salt_mine_view;
  } else {
    return;
  }

  SaltModel *model = dynamic_cast <SaltModel *> (view->model ());
  if (! model) {
    return;
  }

  if (text.isEmpty ()) {

    for (int i = model->rowCount (QModelIndex ()); i > 0; ) {
      --i;
      view->setRowHidden (i, false);
    }

  } else {

    QRegExp re (text, Qt::CaseInsensitive);

    for (int i = model->rowCount (QModelIndex ()); i > 0; ) {
      --i;
      QModelIndex index = model->index (i, 0, QModelIndex ());
      SaltGrain *g = model->grain_from_index (index);
      bool hidden = (!g || re.indexIn (tl::to_qstring (g->name ())) < 0);
      view->setRowHidden (i, hidden);
    }

  }
}

void
SaltManagerDialog::edit_properties ()
{
  SaltGrain *g = current_grain ();
  if (g) {
    if (mp_properties_dialog->exec_dialog (g, mp_salt)) {
      current_changed ();
    }
  }
}

void
SaltManagerDialog::create_grain ()
{
BEGIN_PROTECTED

  SaltGrainTemplateSelectionDialog temp_dialog (this, mp_salt);
  if (temp_dialog.exec ()) {

    SaltGrain target;
    target.set_name (temp_dialog.name ());

    if (mp_salt->create_grain (temp_dialog.templ (), target)) {

      //  select the new one
      SaltModel *model = dynamic_cast <SaltModel *> (salt_view->model ());
      if (model) {
        for (int i = model->rowCount (QModelIndex ()); i > 0; ) {
          --i;
          QModelIndex index = model->index (i, 0, QModelIndex ());
          SaltGrain *g = model->grain_from_index (index);
          if (g && g->name () == target.name ()) {
            salt_view->setCurrentIndex (index);
            break;
          }
        }

      }

    } else {
      throw tl::Exception (tl::to_string (tr ("Initialization of new package failed - see log window (File/Log Viewer) for details")));
    }

  }

END_PROTECTED
}

void
SaltManagerDialog::delete_grain ()
{
BEGIN_PROTECTED

  SaltGrain *g = current_grain ();
  if (! g) {
    throw tl::Exception (tl::to_string (tr ("No package selected to delete")));
  }

  if (QMessageBox::question (this, tr ("Delete Package"), tr ("Are you sure to delete package '%1'?").arg (tl::to_qstring (g->name ())), QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes) {
    mp_salt->remove_grain (*g);
  }

END_PROTECTED
}

void
SaltManagerDialog::salt_changed ()
{
  SaltModel *model = dynamic_cast <SaltModel *> (salt_view->model ());
  if (! model) {
    return;
  }

  //  NOTE: the disabling of the event handler prevents us from
  //  letting the model connect to the salt's signal directly.
  m_current_changed_enabled = false;
  model->update ();
  m_current_changed_enabled = true;

  if (mp_salt->is_empty ()) {

    list_stack->setCurrentIndex (1);
    details_frame->hide ();

  } else {

    list_stack->setCurrentIndex (0);
    details_frame->show ();

    //  select the first grain
    if (model->rowCount (QModelIndex ()) > 0) {
      salt_view->setCurrentIndex (model->index (0, 0, QModelIndex ()));
    }

  }

  current_changed ();
}

void
SaltManagerDialog::current_changed ()
{
  SaltGrain *g = current_grain ();
  details_text->set_grain (g);
  if (!g) {
    details_frame->setEnabled (false);
    delete_button->setEnabled (false);
  } else {
    details_frame->setEnabled (true);
    delete_button->setEnabled (true);
    edit_button->setEnabled (! g->is_readonly ());
  }
}

lay::SaltGrain *
SaltManagerDialog::current_grain ()
{
  SaltModel *model = dynamic_cast <SaltModel *> (salt_view->model ());
  return model ? model->grain_from_index (salt_view->currentIndex ()) : 0;
}

void
SaltManagerDialog::salt_mine_changed ()
{
  SaltModel *model = dynamic_cast <SaltModel *> (salt_mine_view->model ());
  if (! model) {
    return;
  }

  //  NOTE: the disabling of the event handler prevents us from
  //  letting the model connect to the salt's signal directly.
  m_current_changed_enabled = false;
  model->update ();
  m_current_changed_enabled = true;

  //  select the first grain
  if (model->rowCount (QModelIndex ()) > 0) {
    salt_mine_view->setCurrentIndex (model->index (0, 0, QModelIndex ()));
  }

  mine_current_changed ();
}

void
SaltManagerDialog::mine_current_changed ()
{
BEGIN_PROTECTED

  SaltGrain *g = mine_current_grain ();
  details_new_frame->setEnabled (g != 0);

  if (! g) {
    details_new_text->set_grain (0);
    return;
  }

  m_remote_grain.reset (0);

  //  Download actual grain definition file
  try {

    if (g->url ().empty ()) {
      throw tl::Exception (tl::to_string (tr ("No download link available")));
    }

    tl::InputHttpStream http (SaltGrain::spec_url (g->url ()));
    tl::InputStream stream (http);

    m_remote_grain.reset (new SaltGrain ());
    m_remote_grain->load (stream);
    m_remote_grain->set_url (g->url ());

    if (g->name () != m_remote_grain->name ()) {
      throw tl::Exception (tl::to_string (tr ("Name mismatch between repository and actual package (repository: %1, package: %2)").arg (tl::to_qstring (g->name ())).arg (tl::to_qstring (m_remote_grain->name ()))));
    }
    if (SaltGrain::compare_versions (g->version (), m_remote_grain->version ()) != 0) {
      throw tl::Exception (tl::to_string (tr ("Version mismatch between repository and actual package (repository: %1, package: %2)").arg (tl::to_qstring (g->version ())).arg (tl::to_qstring (m_remote_grain->version ()))));
    }

    details_new_text->set_grain (m_remote_grain.get ());

  } catch (tl::Exception &ex) {

    m_remote_grain.reset (0);

    QString text = tr (
      "<html>"
        "<body>"
          "<font color=\"#ff0000\">"
          "<h2>Error Fetching Package Definition</h2>"
          "<p><b>URL</b>: %1</p>"
          "<p><b>Error</b>: %2</p>"
        "</body>"
      "</html>"
    )
    .arg (tl::to_qstring (SaltGrain::spec_url (g->url ())))
    .arg (tl::to_qstring (tl::escaped_to_html (ex.msg ())));

    details_new_text->setHtml (text);

  }

END_PROTECTED
}

lay::SaltGrain *
SaltManagerDialog::mine_current_grain ()
{
  SaltModel *model = dynamic_cast <SaltModel *> (salt_mine_view->model ());
  return model ? model->grain_from_index (salt_mine_view->currentIndex ()) : 0;
}

}
